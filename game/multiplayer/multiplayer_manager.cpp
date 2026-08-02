#include "multiplayer_manager.h"

#include <chrono>

#include "multiplayer_packet.h"
#include "multiplayer_port_mapping.h"
#include "multiplayer_protocol.h"
#include "multiplayer_session.h"

#include "common/cross_sockets/XSocket.h"
#include "common/log/log.h"

#include "enet/enet.h"

namespace {
constexpr size_t kMaxGameplayPeers = 8;
constexpr auto kPortMappingRefreshInterval = std::chrono::hours(1);
constexpr auto kPortMappingRefreshRetryDelay = std::chrono::seconds(5);
constexpr int kPortMappingRefreshAttempts = 3;

bool wait_for_mapping_stop(MultiplayerData& data, std::chrono::milliseconds duration) {
  std::unique_lock<std::mutex> lock(data.port_mapping_mutex);
  return data.port_mapping_cv.wait_for(lock, duration,
                                       [&data]() { return data.port_mapping_worker_stop.load(); });
}

void start_port_mapping_worker(MultiplayerData& data, uint16_t local_port, uint16_t external_port) {
  if (data.port_mapping_thread.joinable()) {
    data.port_mapping_worker_stop = true;
    data.port_mapping_cv.notify_all();
    data.port_mapping_thread.join();
  }

  data.port_mapping_worker_stop = false;
  {
    std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
    data.port_mapping_state = MPPortMappingState::PENDING;
    data.port_mapping_method = MPPortMappingMethod::NONE;
    data.port_mapping_local_port = local_port;
    data.port_mapping_external_port = external_port;
    data.port_mapping_external_ip.clear();
  }

  data.port_mapping_thread = std::thread([&data, local_port, external_port]() {
    if (wait_for_mapping_stop(data, std::chrono::seconds(1))) {
      return;
    }

    auto mapping = mp_open_udp_port_mapping(local_port, external_port);
    if (data.port_mapping_worker_stop) {
      if (mapping.success) {
        mp_close_udp_port_mapping(mapping.method, local_port, external_port);
      }
      return;
    }

    const bool usable_mapping = mapping.success && mp_is_public_ipv4(mapping.external_ip);
    if (mapping.success && !usable_mapping) {
      mp_close_udp_port_mapping(mapping.method, local_port, external_port);
      mapping.success = false;
    }

    {
      std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
      data.port_mapping_state = usable_mapping ? MPPortMappingState::READY
                                               : MPPortMappingState::FAILED;
      data.port_mapping_method = usable_mapping ? mapping.method : MPPortMappingMethod::NONE;
      data.port_mapping_local_port = local_port;
      data.port_mapping_external_port = external_port;
      data.port_mapping_external_ip = usable_mapping ? mapping.external_ip : std::string();
    }

    if (usable_mapping) {
      lg::info("[Multiplayer] Temporary UDP port mapping active for port {}.", external_port);
    } else {
      lg::warn("[Multiplayer] Automatic UDP port mapping failed.");
      return;
    }

    bool refresh_failed = false;
    while (!wait_for_mapping_stop(data, std::chrono::duration_cast<std::chrono::milliseconds>(
                                            kPortMappingRefreshInterval))) {
      if (mapping.method != MPPortMappingMethod::NAT_PMP) {
        continue;
      }
      bool refreshed = false;
      for (int attempt = 0; attempt < kPortMappingRefreshAttempts && !refreshed; ++attempt) {
        refreshed = mp_refresh_udp_port_mapping(mapping.method, local_port, external_port);
        if (!refreshed && attempt + 1 < kPortMappingRefreshAttempts &&
            wait_for_mapping_stop(data, kPortMappingRefreshRetryDelay)) {
          break;
        }
      }
      if (!refreshed) {
        refresh_failed = !data.port_mapping_worker_stop;
        break;
      }
    }

    mp_close_udp_port_mapping(mapping.method, local_port, external_port);
    lg::info("[Multiplayer] Temporary UDP port mapping removed.");
    std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
    data.port_mapping_state = refresh_failed ? MPPortMappingState::FAILED
                                             : MPPortMappingState::IDLE;
    data.port_mapping_method = MPPortMappingMethod::NONE;
    data.port_mapping_external_ip.clear();
    if (refresh_failed) {
      lg::warn("[Multiplayer] Automatic UDP port mapping refresh failed.");
    }
  });
}

void stop_port_mapping_worker(MultiplayerData& data) {
  data.port_mapping_worker_stop = true;
  data.port_mapping_cv.notify_all();
  if (data.port_mapping_thread.joinable()) {
    data.port_mapping_thread.join();
  }
  std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
  data.port_mapping_state = MPPortMappingState::IDLE;
  data.port_mapping_method = MPPortMappingMethod::NONE;
  data.port_mapping_external_ip.clear();
}
}  // namespace

void MultiplayerManager::setup_host(MultiplayerData& data, bool internet_host) {
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

  if (!data.security.start_host(address.port)) {
    lg::error("[Multiplayer] Could not initialize secure handshake.");
    return;
  }
  if (!data.security.set_local_version(data.local_version)) {
    lg::error("[Multiplayer] Cannot host without a valid mod version.");
    data.security.reset();
    data.join_status = (int)MultiplayerStatus::FAILED;
    return;
  }

  data.host = enet_host_create(&address, kMaxGameplayPeers, 2, 0, 0);
  if (data.host) {
    lg::info("[Multiplayer] Listen server started on port {}.", address.port);

    data.local_role = 0;
    data.local_net_id = 0;
    data.authenticated_peer = nullptr;
    data.host_game_active = false;
    data.pending_handshakes = {};
    data.authentication_failures = {};
    data.next_authentication_failure_slot = 0;
    data.internet_host = internet_host;
    data.join_status = (int)MultiplayerStatus::CONNECTING;  // Waiting for peer
    data.initialized = true;

    // Start discovery responder
    data.host_discovery_active = true;
    data.discovery_thread = std::thread(discovery_responder_func, &data);
    if (internet_host) {
      start_port_mapping_worker(data, address.port, address.port);
    }
  } else {
    data.security.reset();
  }
}

bool MultiplayerManager::retry_online_setup(MultiplayerData& data) {
  uint16_t local_port = 0;
  uint16_t external_port = 0;
  {
    std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
    if (!data.initialized || data.local_role != 0 || !data.internet_host ||
        data.port_mapping_state != MPPortMappingState::FAILED) {
      return false;
    }
    local_port = data.port_mapping_local_port;
    external_port = data.port_mapping_external_port;
  }
  if (local_port == 0 || external_port == 0) {
    return false;
  }
  start_port_mapping_worker(data, local_port, external_port);
  return true;
}

int multiplayer_host_invite_status(MultiplayerData& data) {
  if (!data.initialized || data.local_role != 0 || data.security.invite_token().empty()) {
    return 0;
  }
  if (!data.internet_host) {
    return 1;
  }
  std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
  if (data.port_mapping_state == MPPortMappingState::FAILED) {
    return -1;
  }
  return data.port_mapping_state == MPPortMappingState::READY &&
                 !data.port_mapping_external_ip.empty()
             ? 1
             : 0;
}

void MultiplayerManager::setup_client(MultiplayerData& data, const char* ip, int port) {
  if (data.host)
    disconnect(data);

  if (!data.enet_initialized) {
    if (enet_initialize() != 0)
      return;
    data.enet_initialized = true;
  }

  if (!data.security.set_local_version(data.local_version)) {
    lg::error("[Multiplayer] Cannot connect without a valid mod version.");
    data.join_status = (int)MultiplayerStatus::FAILED;
    return;
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
  data.packet_scheduler.clear();
  data.stop_search = true;
  data.host_discovery_active = false;
  if (data.scanner_thread.joinable()) {
    data.scanner_thread.join();
  }
  if (data.discovery_thread.joinable()) {
    data.discovery_thread.join();
  }

  stop_port_mapping_worker(data);

  if (!data.initialized) {
    data.security.reset();
    multiplayer_clear_session_state(data);
    return;
  }

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
  data.internet_host = false;
  data.host_game_active = false;
  data.handshake_started_time = 0;
  data.authenticated_peer = nullptr;
  data.pending_handshakes = {};
  data.authentication_failures = {};
  data.next_authentication_failure_slot = 0;
  data.security.reset();
  data.join_status = (int)MultiplayerStatus::IDLE;
  multiplayer_clear_session_state(data);
  lg::info("[Multiplayer] Disconnected.");
}

bool MultiplayerManager::broadcast(MultiplayerData& data,
                                   int channel,
                                   const void* packet_data,
                                   size_t size,
                                   ENetPacketFlag flags) {
  return mp_send_packet(data, channel, packet_data, size, flags);
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
  if (sock < 0)
    return;

  sockaddr_in listen_addr;
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_port = htons(DISCOVERY_PORT);
  listen_addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(sock, (sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
    lg::error("[Multiplayer] Discovery responder failed to bind to port {}", DISCOVERY_PORT);
    close_socket(sock);
    return;
  }

  set_socket_timeout(sock, 1000000);  // 1s timeout for checking stop flag

  lg::info("[Multiplayer] Discovery responder active on port {}", DISCOVERY_PORT);

  char buffer[64];
  while (data->host_discovery_active) {
    sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    int bytes_received =
        recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&from_addr, &from_len);

    if (bytes_received > 0) {
      buffer[bytes_received] = '\0';
      if (std::string(buffer) == DISCOVERY_MAGIC) {
        std::string reply =
            std::string(DISCOVERY_MAGIC) + "|" + data->security.invite_token();
        sendto(sock, reply.c_str(), reply.size(), 0, (sockaddr*)&from_addr, from_len);
        mp_secure_clear_string(reply);
      }
    }
  }

  lg::info("[Multiplayer] Discovery responder stopped.");
  close_socket(sock);
}
