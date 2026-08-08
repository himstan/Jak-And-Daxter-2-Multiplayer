#include "multiplayer_manager.h"

#include <chrono>
#include <string>
#include <utility>

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
constexpr int kOrderlyDisconnectDrainAttempts = 10;

std::string enet_endpoint_string(const ENetAddress& address) {
  char host[64] = {};
  if (enet_address_get_host_ip(&address, host, sizeof(host)) != 0) {
    return "<unknown>:" + std::to_string(address.port);
  }
  return std::string(host) + ":" + std::to_string(address.port);
}

std::string enet_peer_endpoint_string(const ENetPeer* peer) {
  return peer ? enet_endpoint_string(peer->address) : "<none>";
}

uint16_t enet_local_port(const ENetHost* host) {
  if (!host) {
    return 0;
  }
  if (host->address.port != 0) {
    return host->address.port;
  }
  ENetAddress local = {};
  if (enet_socket_get_address(host->socket, &local) != 0) {
    return 0;
  }
  return local.port;
}

const char* port_mapping_method_name(MPPortMappingMethod method) {
  switch (method) {
    case MPPortMappingMethod::UPNP_IGD:
      return "UPnP IGD";
    case MPPortMappingMethod::NAT_PMP:
      return "NAT-PMP";
    case MPPortMappingMethod::NONE:
      return "automatic port mapping";
  }
  return "unknown port-mapping method";
}

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
        mp_close_udp_port_mapping(mapping, local_port, external_port);
      }
      return;
    }

    const bool usable_mapping = mapping.success && mp_is_public_ipv4(mapping.external_ip);
    if (mapping.success && !usable_mapping) {
      mp_close_udp_port_mapping(mapping, local_port, external_port);
      const std::string method_name = port_mapping_method_name(mapping.method);
      if (mapping.external_ip.empty()) {
        mapping.error =
            mapping.error.empty()
                ? method_name +
                      " mapping succeeded, but the router returned no external IPv4 address"
                : method_name + ": " + mapping.error;
      } else {
        mapping.error = method_name + " mapping returned a non-public external IPv4 address (" +
                        mapping.external_ip + ")";
      }
      mapping.success = false;
    }

    {
      std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
      data.port_mapping_state =
          usable_mapping ? MPPortMappingState::READY : MPPortMappingState::FAILED;
      data.port_mapping_method = usable_mapping ? mapping.method : MPPortMappingMethod::NONE;
      data.port_mapping_local_port = local_port;
      data.port_mapping_external_port = external_port;
      data.port_mapping_external_ip = usable_mapping ? mapping.external_ip : std::string();
    }

    if (usable_mapping) {
      lg::info("[Multiplayer] Temporary UDP port mapping active for port {}.", external_port);
    } else {
      lg::warn("[Multiplayer] Automatic UDP port mapping failed: {}.",
               mapping.error.empty() ? "unknown failure" : mapping.error);
      return;
    }

    bool refresh_failed = false;
    std::string refresh_error;
    while (!wait_for_mapping_stop(
        data, std::chrono::duration_cast<std::chrono::milliseconds>(kPortMappingRefreshInterval))) {
      if (mapping.method != MPPortMappingMethod::NAT_PMP) {
        continue;
      }
      bool refreshed = false;
      for (int attempt = 0; attempt < kPortMappingRefreshAttempts && !refreshed; ++attempt) {
        auto refresh = mp_refresh_udp_port_mapping(mapping, local_port, external_port);
        refreshed = refresh.success;
        if (!refreshed) {
          refresh_error = std::move(refresh.error);
        }
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

    mp_close_udp_port_mapping(mapping, local_port, external_port);
    lg::info("[Multiplayer] Temporary UDP port mapping removed.");
    std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
    data.port_mapping_state =
        refresh_failed ? MPPortMappingState::FAILED : MPPortMappingState::IDLE;
    data.port_mapping_method = MPPortMappingMethod::NONE;
    data.port_mapping_external_ip.clear();
    if (refresh_failed) {
      lg::warn("[Multiplayer] Automatic UDP port mapping refresh failed: {}.",
               refresh_error.empty() ? "unknown failure" : refresh_error);
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

void disconnect_peer_after_leave(ENetHost* host,
                                 ENetPeer* peer,
                                 uint32_t disconnect_reason,
                                 bool leave_sent) {
  if (!host || !peer) {
    lg::warn("[MP-Leave] Teardown skipped: host or peer unavailable (host={}, peer={}).",
             host != nullptr, peer != nullptr);
    return;
  }
  if (peer->state == ENET_PEER_STATE_DISCONNECTED) {
    lg::info("[MP-Leave] Teardown skipped: peer {} was already disconnected (reason={}).",
             enet_peer_endpoint_string(peer), disconnect_reason);
    return;
  }

  const auto endpoint = enet_peer_endpoint_string(peer);
  int drain_iterations = 0;
  int receive_events = 0;
  int disconnect_events = 0;
  bool forced_disconnect = false;
  lg::info(
      "[MP-Leave] Beginning peer teardown for {} (leave_sent={}, reason={}, initial_state={}, "
      "reliable_in_transit={}, waiting_data={}, outgoing_data={}, packets_sent={}).",
      endpoint, leave_sent, disconnect_reason, static_cast<int>(peer->state),
      peer->reliableDataInTransit, peer->totalWaitingData, peer->outgoingDataTotal,
      peer->packetsSent);

  if (leave_sent) {
    enet_peer_disconnect_later(peer, disconnect_reason);
    for (int attempt = 0;
         attempt < kOrderlyDisconnectDrainAttempts && peer->state != ENET_PEER_STATE_DISCONNECTED;
         ++attempt) {
      drain_iterations = attempt + 1;
      ENetEvent event = {};
      enet_host_service(host, &event, 5);
      if (event.type == ENET_EVENT_TYPE_RECEIVE && event.packet) {
        ++receive_events;
        enet_packet_destroy(event.packet);
      } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
        ++disconnect_events;
      }
    }
  }

  if (peer->state != ENET_PEER_STATE_DISCONNECTED) {
    forced_disconnect = true;
    enet_peer_disconnect_now(peer, disconnect_reason);
  }
  lg::info(
      "[MP-Leave] Peer teardown complete for {} (drain_iterations={}, received_during_drain={}, "
      "disconnect_events={}, forced_disconnect={}, final_state={}, reliable_in_transit={}, "
      "waiting_data={}, outgoing_data={}, packets_sent={}).",
      endpoint, drain_iterations, receive_events, disconnect_events, forced_disconnect,
      static_cast<int>(peer->state), peer->reliableDataInTransit, peer->totalWaitingData,
      peer->outgoingDataTotal, peer->packetsSent);
}

bool send_leave_notice(MultiplayerData& data, MultiplayerLeaveReason reason) {
  if (!data.initialized || !data.host || !data.security.authenticated()) {
    lg::warn(
        "[MP-Leave] Could not send EVENT_LEAVE (reason {}): authenticated transport unavailable "
        "(initialized={}, host={}, security_authenticated={}, queued_packets={}, queued_bytes={}).",
        static_cast<int>(reason), data.initialized, data.host != nullptr,
        data.security.authenticated(), data.packet_scheduler.queued_packet_count(),
        data.packet_scheduler.queued_byte_count());
    return false;
  }

  ENetPeer* peer = data.local_role == 0 ? data.authenticated_peer : data.server_peer;
  if (!peer || peer->state != ENET_PEER_STATE_CONNECTED) {
    lg::warn(
        "[MP-Leave] Could not send EVENT_LEAVE (reason {}): peer unavailable (peer={}, state={}, "
        "queued_packets={}, queued_bytes={}).",
        static_cast<int>(reason), enet_peer_endpoint_string(peer),
        peer ? static_cast<int>(peer->state) : -1, data.packet_scheduler.queued_packet_count(),
        data.packet_scheduler.queued_byte_count());
    return false;
  }

  lg::info(
      "[MP-Leave] Sending EVENT_LEAVE directly to {} (role={}, reason={}, peer_state={}, "
      "queued_packets={}, queued_bytes={}, reliable_in_transit={}, waiting_data={}, "
      "outgoing_data={}, packets_sent={}).",
      enet_peer_endpoint_string(peer), data.local_role, static_cast<int>(reason),
      static_cast<int>(peer->state), data.packet_scheduler.queued_packet_count(),
      data.packet_scheduler.queued_byte_count(), peer->reliableDataInTransit,
      peer->totalWaitingData, peer->outgoingDataTotal, peer->packetsSent);
  PacketLeave leave = {{PacketType::EVENT_LEAVE, ++data.sequence_num}, reason};
  if (!mp_send_packet_immediately(data, peer, static_cast<int>(MultiplayerChannel::CONTROL), &leave,
                                  sizeof(leave), ENET_PACKET_FLAG_RELIABLE)) {
    lg::warn("[MP-Leave] Could not encrypt or send EVENT_LEAVE (reason={}) to {}.",
             static_cast<int>(reason), enet_peer_endpoint_string(peer));
    return false;
  }

  const auto reliable_before_flush = peer->reliableDataInTransit;
  const auto waiting_before_flush = peer->totalWaitingData;
  enet_host_flush(data.host);
  lg::info(
      "[MP-Leave] EVENT_LEAVE accepted locally by ENet (reason={}, peer={}, "
      "reliable_before_flush={}, waiting_before_flush={}, reliable_after_flush={}, "
      "waiting_after_flush={}, packets_sent={}); this does not confirm remote delivery.",
      static_cast<int>(reason), enet_peer_endpoint_string(peer), reliable_before_flush,
      waiting_before_flush, peer->reliableDataInTransit, peer->totalWaitingData, peer->packetsSent);
  return true;
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
  lg::info(
      "[MP-Reconnect] Creating client transport for target {}:{} (reconnect_active={}, attempt={}, "
      "status={}).",
      ip ? ip : "<null>", port, data.reconnect_attempt_active, data.reconnect_attempt_count,
      data.join_status.load());
  if (data.host)
    disconnect(data);

  if (!data.enet_initialized) {
    if (enet_initialize() != 0) {
      lg::error("[MP-Reconnect] ENet initialization failed while creating client transport.");
      return;
    }
    data.enet_initialized = true;
  }

  if (!data.security.set_local_version(data.local_version)) {
    lg::error("[Multiplayer] Cannot connect without a valid mod version.");
    data.join_status = (int)MultiplayerStatus::FAILED;
    return;
  }

  data.host = enet_host_create(NULL, 1, 2, 0, 0);
  if (!data.host) {
    lg::error("[MP-Reconnect] ENet client host creation failed for target {}:{}.",
              ip ? ip : "<null>", port);
    return;
  }

  ENetAddress server_address = {};
  const int address_result = enet_address_set_host(&server_address, ip);
  server_address.port = port;
  lg::info(
      "[MP-Reconnect] Client UDP socket created on local port {} (target resolve_result={}, "
      "parsed_target={}:{}).",
      enet_local_port(data.host), address_result, ip ? ip : "<null>", server_address.port);
  if (address_result != 0) {
    lg::warn(
        "[MP-Reconnect] Target address resolution failed for {}:{}; ENet connect will report the "
        "final result.",
        ip ? ip : "<null>", port);
  }

  data.server_peer = enet_host_connect(data.host, &server_address, 2, 0);
  if (data.server_peer) {
    lg::info(
        "[MP-Reconnect] ENet peer created for {}:{} (local_port={}, peer_state={}, "
        "reconnect_active={}); connect event pending.",
        ip ? ip : "<null>", server_address.port, enet_local_port(data.host),
        static_cast<int>(data.server_peer->state), data.reconnect_attempt_active);
    lg::info("[Multiplayer] Client connecting...");
    data.local_role = 1;
    data.local_net_id = 1;
    data.join_status = (int)MultiplayerStatus::CONNECTING;
    data.initialized = true;
  } else {
    lg::error("[MP-Reconnect] ENet peer creation failed for {}:{} (local_port={}).",
              ip ? ip : "<null>", server_address.port, enet_local_port(data.host));
    enet_host_destroy(data.host);
    data.host = nullptr;
  }
}

void MultiplayerManager::disconnect(MultiplayerData& data, bool preserve_reconnect_state) {
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
    data.packet_scheduler.clear();
    data.security.reset();
    multiplayer_clear_session_state(data, preserve_reconnect_state);
    if (preserve_reconnect_state) {
      data.join_status = (int)MultiplayerStatus::RECONNECTING;
    }
    return;
  }

  if (data.host) {
    if (data.local_role == 1 && data.server_peer) {
      const bool leave_sent = send_leave_notice(
          data, preserve_reconnect_state ? MultiplayerLeaveReason::CLIENT_RECONNECTING
                                         : MultiplayerLeaveReason::CLIENT_CLOSED);
      disconnect_peer_after_leave(data.host, data.server_peer,
                                  preserve_reconnect_state ? 0 : kDisconnectReasonClientClosed,
                                  leave_sent);
    } else if (data.local_role == 0) {
      const bool leave_sent = send_leave_notice(data, MultiplayerLeaveReason::HOST_CLOSED);
      ENetPeer* authenticated_peer = data.authenticated_peer;
      for (size_t i = 0; i < data.host->peerCount; ++i) {
        ENetPeer* peer = &data.host->peers[i];
        if (peer->state == ENET_PEER_STATE_CONNECTED) {
          disconnect_peer_after_leave(data.host, peer, kDisconnectReasonHostClosed,
                                      peer == authenticated_peer && leave_sent);
        }
      }
    }
    enet_host_destroy(data.host);
    data.host = nullptr;
  }

  data.packet_scheduler.clear();

  data.initialized = false;
  data.internet_host = false;
  data.host_game_active = false;
  data.handshake_started_time = 0;
  data.authenticated_peer = nullptr;
  data.pending_handshakes = {};
  data.authentication_failures = {};
  data.next_authentication_failure_slot = 0;
  data.security.reset();
  data.join_status = preserve_reconnect_state ? (int)MultiplayerStatus::RECONNECTING
                                              : (int)MultiplayerStatus::IDLE;
  multiplayer_clear_session_state(data, preserve_reconnect_state);
  if (!preserve_reconnect_state) {
    lg::info("[Multiplayer] Disconnected.");
  }
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
        std::string reply = std::string(DISCOVERY_MAGIC) + "|" + data->security.invite_token();
        sendto(sock, reply.c_str(), reply.size(), 0, (sockaddr*)&from_addr, from_len);
        mp_secure_clear_string(reply);
      }
    }
  }

  lg::info("[Multiplayer] Discovery responder stopped.");
  close_socket(sock);
}
