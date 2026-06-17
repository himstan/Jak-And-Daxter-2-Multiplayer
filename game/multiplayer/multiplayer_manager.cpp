#include "multiplayer_manager.h"
#include "multiplayer_protocol.h"
#include "multiplayer_packet.h"
#include "multiplayer_port_mapping.h"
#include "multiplayer_session.h"
#include "common/log/log.h"
#include "common/cross_sockets/XSocket.h"
#include "enet/enet.h"

#include <chrono>

namespace {
constexpr size_t kMaxGameplayPeers = 1;

void start_port_mapping_worker(MultiplayerData& data, uint16_t local_port, uint16_t external_port) {
  if (data.port_mapping_thread.joinable()) {
    data.port_mapping_worker_stop = true;
    data.port_mapping_thread.join();
  }

  data.port_mapping_worker_stop = false;
  const uint32_t worker_generation = ++data.port_mapping_generation;
  {
    std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
    data.port_mapping_active = false;
    data.port_mapping_method = MPPortMappingMethod::NONE;
    data.port_mapping_local_port = local_port;
    data.port_mapping_external_port = external_port;
    data.last_port_mapping_refresh_time = 0;
  }

  data.port_mapping_thread = std::thread([&data, local_port, external_port, worker_generation]() {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (data.port_mapping_worker_stop || !data.initialized || data.local_role != 0) {
      return;
    }

    auto mapping = mp_open_udp_port_mapping(local_port, external_port);
    if (data.port_mapping_worker_stop || data.port_mapping_generation.load() != worker_generation ||
        !data.initialized || data.local_role != 0) {
      if (mapping.success) {
        mp_close_udp_port_mapping(mapping.method, local_port, external_port);
      }
      return;
    }

    {
      std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
      data.port_mapping_active = mapping.success;
      data.port_mapping_method = mapping.method;
      data.port_mapping_local_port = local_port;
      data.port_mapping_external_port = external_port;
      data.last_port_mapping_refresh_time = enet_time_get();
    }

    if (mapping.success) {
      lg::info("[Multiplayer] Temporary UDP port mapping active for port {}.", external_port);
    } else {
      lg::warn("[Multiplayer] Automatic UDP port mapping failed: {}", mapping.error);
    }
  });
}

void stop_port_mapping_worker_async(MultiplayerData& data) {
  data.port_mapping_worker_stop = true;
  ++data.port_mapping_generation;
  if (data.port_mapping_thread.joinable()) {
    data.port_mapping_thread.detach();
  }
}

void close_port_mapping_async(MPPortMappingMethod method,
                              uint16_t local_port,
                              uint16_t external_port) {
  std::thread([method, local_port, external_port]() {
    mp_close_udp_port_mapping(method, local_port, external_port);
    lg::info("[Multiplayer] Temporary UDP port mapping removed.");
  }).detach();
}
}

void MultiplayerManager::setup_host(MultiplayerData& data) {
  if (data.host)
    disconnect(data);

  if (!data.enet_initialized) {
    if (enet_initialize() != 0)
      return;
    data.enet_initialized = true;
  }

  ENetAddress address;
  address.host = ENET_HOST_ANY;
  address.port = 26210;

  data.host = enet_host_create(&address, kMaxGameplayPeers, 2, 0, 0);
  if (data.host) {
    lg::info("[Multiplayer] Listen server started on port {}.", address.port);

    data.local_role = 0;
    data.local_net_id = 0;
    data.join_status = (int)MultiplayerStatus::CONNECTING; // Waiting for peer
    data.initialized = true;

    // Start discovery responder
    data.host_discovery_active = true;
    data.discovery_thread = std::thread(discovery_responder_func, &data);
    start_port_mapping_worker(data, address.port, address.port);
  }
}

void MultiplayerManager::setup_client(MultiplayerData& data, const char* ip, int port) {
  if (data.host)
    disconnect(data);

  if (!data.enet_initialized) {
    if (enet_initialize() != 0)
      return;
    data.enet_initialized = true;
  }

  data.host = enet_host_create(NULL, 1, 2, 0, 0);
  if (data.host) {
    ENetAddress server_address;
    enet_address_set_host(&server_address, ip);
    server_address.port = port;

    data.server_peer = enet_host_connect(data.host, &server_address, 2, 0);
    if (data.server_peer) {
      lg::info("[Multiplayer] Client connecting...");
      data.local_role = 1;
      data.local_net_id = 1;
      data.join_status = (int)MultiplayerStatus::CONNECTING;
      data.initialized = true;
    } else {
      enet_host_destroy(data.host);
      data.host = nullptr;
    }
  }
}

void MultiplayerManager::disconnect(MultiplayerData& data) {
  data.stop_search = true;
  data.host_discovery_active = false;
  if (data.scanner_thread.joinable()) {
    data.scanner_thread.join();
  }
  if (data.discovery_thread.joinable()) {
    data.discovery_thread.join();
  }

  MPPortMappingMethod mapping_method = MPPortMappingMethod::NONE;
  uint16_t mapping_local_port = 0;
  uint16_t mapping_external_port = 0;
  bool had_mapping = false;
  {
    std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
    had_mapping = data.port_mapping_active;
    mapping_method = data.port_mapping_method;
    mapping_local_port = data.port_mapping_local_port;
    mapping_external_port = data.port_mapping_external_port;
    data.port_mapping_active = false;
    data.port_mapping_method = MPPortMappingMethod::NONE;
    data.port_mapping_local_port = 0;
    data.port_mapping_external_port = 0;
    data.last_port_mapping_refresh_time = 0;
  }
  stop_port_mapping_worker_async(data);
  if (had_mapping) {
    close_port_mapping_async(mapping_method, mapping_local_port, mapping_external_port);
  }

  if (!data.initialized)
    return;

  if (data.host) {
    if (data.local_role == 1 && data.server_peer) {
      enet_peer_disconnect_now(data.server_peer, 0);
    } else if (data.local_role == 0) {
      // Host disconnecting: notify all peers
      for (size_t i = 0; i < data.host->peerCount; ++i) {
        ENetPeer* peer = &data.host->peers[i];
        if (peer->state == ENET_PEER_STATE_CONNECTED) {
          enet_peer_disconnect_now(peer, 0);
        }
      }
    }
    enet_host_destroy(data.host);
    data.host = nullptr;
  }

  data.initialized = false;
  data.join_status = (int)MultiplayerStatus::IDLE;
  multiplayer_clear_session_state(data);
  lg::info("[Multiplayer] Disconnected.");
}

void MultiplayerManager::broadcast(MultiplayerData& data,
                                   int channel,
                                   const void* packet_data,
                                   size_t size,
                                   ENetPacketFlag flags) {
  mp_send_packet(data, channel, packet_data, size, flags);
}

void MultiplayerManager::send_to_peer(ENetPeer* peer,
                                      int channel,
                                      const void* packet_data,
                                      size_t size,
                                      ENetPacketFlag flags) {
  mp_send_packet_to_peer(peer, channel, packet_data, size, flags);
}

void MultiplayerManager::discovery_responder_func(MultiplayerData* data) {
  int sock = open_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) return;

  sockaddr_in listen_addr;
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_port = htons(DISCOVERY_PORT);
  listen_addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(sock, (sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
    lg::error("[Multiplayer] Discovery responder failed to bind to port {}", DISCOVERY_PORT);
    close_socket(sock);
    return;
  }

  set_socket_timeout(sock, 1000000); // 1s timeout for checking stop flag

  lg::info("[Multiplayer] Discovery responder active on port {}", DISCOVERY_PORT);

  char buffer[64];
  while (data->host_discovery_active) {
    sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    int bytes_received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&from_addr, &from_len);
    
    if (bytes_received > 0) {
      buffer[bytes_received] = '\0';
      if (std::string(buffer) == DISCOVERY_MAGIC) {
        // Send reply
        sendto(sock, DISCOVERY_MAGIC, strlen(DISCOVERY_MAGIC), 0, (sockaddr*)&from_addr, from_len);
      }
    }
  }

  lg::info("[Multiplayer] Discovery responder stopped.");
  close_socket(sock);
}
