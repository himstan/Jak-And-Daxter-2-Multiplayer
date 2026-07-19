#include "multiplayer_api.h"

#include <cstring>
#include <limits>
#include <string>

#ifdef OS_POSIX
#include <netdb.h>
#endif

#include "common/cross_sockets/XSocket.h"
#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/versions/versions.h"

#include "enet/enet.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/multiplayer/multiplayer.h"
#include "game/multiplayer/multiplayer_direct_connect.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_port_mapping.h"
#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/multiplayer_scanner.h"
#include "game/multiplayer/multiplayer_session.h"
#include "game/multiplayer/multiplayer_types.h"
#include "game/multiplayer/multiplayer_version.h"
#include "game/multiplayer/pedestrian/multiplayer_pedestrian.h"
#include "game/multiplayer/sync/airlock_sync.h"
#include "game/multiplayer/sync/enemy_sync.h"
#include "game/multiplayer/sync/event_sync.h"
#include "game/multiplayer/sync/palace_squid_sync.h"
#include "game/multiplayer/sync/player_sync.h"
#include "game/multiplayer/sync/traffic_sync.h"
#include "game/multiplayer/vehicle/multiplayer_vehicle.h"

#include "third-party/SDL/include/SDL3/SDL.h"

namespace {
constexpr u32 kMinGoalPointer = 0x1000;

bool authentication_address_banned(MultiplayerData& data, uint32_t address, uint32_t now);
void record_authentication_failure(MultiplayerData& data, uint32_t address, uint32_t now);
void add_pending_handshake(MultiplayerData& data, ENetPeer* peer, uint32_t now);
void remove_pending_handshake(MultiplayerData& data, ENetPeer* peer);
void expire_pending_handshakes(MultiplayerData& data, uint32_t now);
void disconnect_other_pending_peers(MultiplayerData& data, ENetPeer* authenticated);

std::string local_invite_address() {
  char hostname[256] = {};
  if (gethostname(hostname, sizeof(hostname)) != 0) {
    return "127.0.0.1";
  }
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* addresses = nullptr;
  if (getaddrinfo(hostname, nullptr, &hints, &addresses) != 0) {
    return "127.0.0.1";
  }

  std::string result = "127.0.0.1";
  for (const addrinfo* address = addresses; address; address = address->ai_next) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->ai_addr);
    const uint32_t host_order = ntohl(ipv4->sin_addr.s_addr);
    if ((host_order >> 24) == 127 || host_order == 0) {
      continue;
    }
    char buffer[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer))) {
      result = buffer;
      break;
    }
  }
  freeaddrinfo(addresses);
  return result;
}

template <typename T>
T* goal_ptr(u32 ptr) {
  if (ptr < kMinGoalPointer || ptr > static_cast<u32>(EE_MAIN_MEM_SIZE - sizeof(T))) {
    return nullptr;
  }
  return Ptr<T>(ptr).c();
}

const char* goal_string_data(u32 ptr) {
  if (ptr < kMinGoalPointer || ptr > static_cast<u32>(EE_MAIN_MEM_SIZE - sizeof(String) - 1)) {
    return nullptr;
  }
  auto* str = Ptr<String>(ptr).c();
  if (!str) {
    return nullptr;
  }
  constexpr u32 kMaxBridgeStringLength = 4096;
  if (str->len > kMaxBridgeStringLength ||
      str->len > static_cast<u32>(EE_MAIN_MEM_SIZE) - ptr - sizeof(String) - 1) {
    return nullptr;
  }
  const char* value = str->data();
  return value[str->len] == '\0' ? value : nullptr;
}

void handle_receive_packet(MultiplayerData& data,
                           const ENetPacket* packet,
                           LocalPlayerInfoGOAL* local,
                           RemotePlayerInfoGOAL* remote,
                           uint32_t current_time) {
  PacketView view(packet);
  if (!view.has_header()) {
    return;
  }

  const PacketType packet_type = view.type();
  const int sender_role = data.local_role == 0 ? 1 : 0;
  if (!mp_packet_direction_allowed(packet_type, sender_role)) {
    return;
  }

  switch (packet_type) {
    case PacketType::STATE_UPDATE:
      mp_handle_player_state_packet(data, packet, remote, current_time);
      break;
    case PacketType::EVENT_GAME:
      mp_handle_game_event_packet(data, packet);
      break;
    case PacketType::ENEMY_SYNC:
      mp_handle_enemy_sync_packet(data, packet, current_time);
      break;
    case PacketType::PEDESTRIAN_SYNC: {
      ENetEvent traffic_event = {};
      traffic_event.type = ENET_EVENT_TYPE_RECEIVE;
      traffic_event.packet = const_cast<ENetPacket*>(packet);
      handle_pedestrian_sync_packet(traffic_event, data);
      break;
    }
    case PacketType::VEHICLE_SYNC: {
      ENetEvent traffic_event = {};
      traffic_event.type = ENET_EVENT_TYPE_RECEIVE;
      traffic_event.packet = const_cast<ENetPacket*>(packet);
      handle_vehicle_sync_packet(traffic_event, data);
      break;
    }
    case PacketType::TURRET_SYNC:
      mp_handle_turret_state_packet(data, packet);
      break;
    case PacketType::PALACE_SQUID_SYNC:
      mp_handle_palace_squid_sync_packet(data, packet, current_time);
      break;
    case PacketType::AIRLOCK_SYNC:
      mp_handle_airlock_sync_packet(data, packet, current_time);
      break;
    case PacketType::FULL_SYNC:
      mp_handle_full_sync_packet(packet, local, remote);
      break;
    case PacketType::EVENT_JOIN:
    case PacketType::EVENT_LEAVE:
    default:
      if (current_time - data.last_traffic_short_packet_debug_time > 2000) {
        lg::warn("[Multiplayer] Ignoring unknown packet type {} ({} bytes).", (int)view.type(),
                 view.size());
        data.last_traffic_short_packet_debug_time = current_time;
      }
      break;
  }
}

void poll_network(MultiplayerData& data, LocalPlayerInfoGOAL* local, RemotePlayerInfoGOAL* remote) {
  data.stats.calculate_rates(data.host);

  ENetEvent event;
  uint32_t current_time = enet_time_get();

  while (enet_host_service(data.host, &event, 0) > 0) {
    if (multiplayer_debug_receive_stopped()) {
      if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        enet_packet_destroy(event.packet);
      }
      continue;
    }

    switch (event.type) {
      case ENET_EVENT_TYPE_RECEIVE: {
        if (data.local_role == 0 && data.authenticated_peer &&
            event.peer != data.authenticated_peer) {
          enet_packet_destroy(event.packet);
          break;
        }
        SecurityReceiveResult secured =
            data.security.receive(data.local_role, event.packet->data, event.packet->dataLength);
        bool response_sent = true;
        if (secured.response.size != 0) {
          response_sent = mp_send_raw_packet_to_peer(
              event.peer, static_cast<int>(MultiplayerChannel::CONTROL),
              secured.response.bytes.data(), secured.response.size, ENET_PACKET_FLAG_RELIABLE);
        }
        if (!response_sent) {
          enet_peer_disconnect_now(event.peer, 0);
        } else if (secured.kind == SecurityReceiveKind::VERSION_MISMATCH) {
          if (data.local_role == 0) {
            lg::info("[Multiplayer] Rejected client with an incompatible mod version.");
            enet_peer_disconnect_later(event.peer, 0);
            remove_pending_handshake(data, event.peer);
          } else {
            data.required_version = data.security.remote_version();
            data.handshake_started_time = 0;
            data.join_status = (int)MultiplayerStatus::VERSION_MISMATCH;
            lg::warn("[Multiplayer] Host requires a different mod version.");
          }
        } else if (secured.kind == SecurityReceiveKind::HANDSHAKE &&
                   data.security.authenticated()) {
          if (data.local_role == 0) {
            data.authenticated_peer = event.peer;
            disconnect_other_pending_peers(data, event.peer);
            if (data.host_game_active) {
              data.join_status = (int)MultiplayerStatus::IN_GAME;
              multiplayer_request_full_sync(data);
            } else {
              data.join_status = (int)MultiplayerStatus::CONNECTED_LOBBY;
            }
          } else {
            data.join_status = (int)MultiplayerStatus::CONNECTED_LOBBY;
          }
          data.handshake_started_time = 0;
          data.last_receive_time = current_time;
          lg::info("[Multiplayer] Secure handshake peer authenticated.");
        } else if (secured.kind == SecurityReceiveKind::GAMEPLAY) {
          ENetPacket plaintext_packet = {};
          plaintext_packet.data = secured.plaintext.bytes.data();
          plaintext_packet.dataLength = secured.plaintext.size;
          data.last_receive_time = current_time;
          data.stats.track_recv_bytes(plaintext_packet.data, plaintext_packet.dataLength);
          if (data.join_status == (int)MultiplayerStatus::RECONNECTING) {
            lg::info(
                "[Multiplayer] Authenticated packet received. Connection restored. Resuming status "
                "{}...",
                data.pre_reconnect_status);
            data.join_status = data.pre_reconnect_status;
          }
          handle_receive_packet(data, &plaintext_packet, local, remote, current_time);
        } else if (secured.kind == SecurityReceiveKind::REJECTED && data.local_role == 0 &&
                   !data.authenticated_peer) {
          record_authentication_failure(data, event.peer->address.host, current_time);
          enet_peer_disconnect_now(event.peer, 0);
          remove_pending_handshake(data, event.peer);
        }
        enet_packet_destroy(event.packet);
        break;
      }
      case ENET_EVENT_TYPE_CONNECT:
        data.last_receive_time = current_time;
        if (data.local_role == 1) {
          data.handshake_started_time = current_time;
          lg::info("[Multiplayer] Transport connected. Awaiting secure authentication.");
        } else if (data.local_role == 0) {
          if (data.authenticated_peer || data.security.authenticated() ||
              authentication_address_banned(data, event.peer->address.host, current_time)) {
            enet_peer_disconnect_now(event.peer, 0);
            break;
          }
          add_pending_handshake(data, event.peer, current_time);
          MultiplayerDatagram hello;
          if (data.security.make_server_hello(hello)) {
            mp_send_raw_packet_to_peer(event.peer, static_cast<int>(MultiplayerChannel::CONTROL),
                                       hello.bytes.data(), hello.size, ENET_PACKET_FLAG_RELIABLE);
          }
          lg::info("[Multiplayer] Client transport connected. Authentication challenge sent.");
        }
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        if (data.local_role == 0) {
          remove_pending_handshake(data, event.peer);
          if (event.peer == data.authenticated_peer) {
            if (multiplayer_prepare_host_for_next_peer(data)) {
              lg::info("[Multiplayer] Authenticated client disconnected; host remains available.");
            } else {
              lg::error("[Multiplayer] Failed to prepare the host for another client.");
            }
          }
        } else {
          lg::warn("[Multiplayer] Host connection closed.");
          data.remote_entity = {};
          if (data.join_status != (int)MultiplayerStatus::VERSION_MISMATCH) {
            data.join_status = (int)MultiplayerStatus::HOST_LEFT;
          }
        }
        break;
      default:
        break;
    }
  }

  if (data.local_role == 0) {
    expire_pending_handshakes(data, current_time);
  } else if (!data.security.authenticated() && data.handshake_started_time != 0 &&
             current_time - data.handshake_started_time > 5000) {
    lg::warn("[Multiplayer] Secure handshake timed out.");
    MultiplayerManager::disconnect(data);
  }
}

MultiplayerData::AuthenticationFailure* authentication_failure_for(MultiplayerData& data,
                                                                   uint32_t address,
                                                                   bool create) {
  for (auto& entry : data.authentication_failures) {
    if (entry.address == address) {
      return &entry;
    }
  }
  if (!create) {
    return nullptr;
  }
  auto& entry = data.authentication_failures[data.next_authentication_failure_slot];
  data.next_authentication_failure_slot =
      (data.next_authentication_failure_slot + 1) % data.authentication_failures.size();
  entry = {};
  entry.address = address;
  return &entry;
}

bool authentication_address_banned(MultiplayerData& data, uint32_t address, uint32_t now) {
  auto* entry = authentication_failure_for(data, address, false);
  return entry && static_cast<int32_t>(entry->banned_until - now) > 0;
}

void record_authentication_failure(MultiplayerData& data, uint32_t address, uint32_t now) {
  auto* entry = authentication_failure_for(data, address, true);
  if (!entry) {
    return;
  }
  if (entry->window_start == 0 || now - entry->window_start > 30000) {
    entry->window_start = now;
    entry->count = 0;
  }
  if (entry->count < UINT8_MAX) {
    ++entry->count;
  }
  if (entry->count >= 3) {
    entry->banned_until = now + 60000;
    entry->window_start = now;
    entry->count = 0;
  }
}

void add_pending_handshake(MultiplayerData& data, ENetPeer* peer, uint32_t now) {
  for (auto& pending : data.pending_handshakes) {
    if (!pending.peer) {
      pending.peer = peer;
      pending.deadline = now + 5000;
      return;
    }
  }
  enet_peer_disconnect_now(peer, 0);
}

void remove_pending_handshake(MultiplayerData& data, ENetPeer* peer) {
  for (auto& pending : data.pending_handshakes) {
    if (pending.peer == peer) {
      pending = {};
      return;
    }
  }
}

void expire_pending_handshakes(MultiplayerData& data, uint32_t now) {
  for (auto& pending : data.pending_handshakes) {
    if (pending.peer && static_cast<int32_t>(now - pending.deadline) >= 0) {
      record_authentication_failure(data, pending.peer->address.host, now);
      enet_peer_disconnect_now(pending.peer, 0);
      pending = {};
    }
  }
}

void disconnect_other_pending_peers(MultiplayerData& data, ENetPeer* authenticated) {
  for (auto& pending : data.pending_handshakes) {
    if (pending.peer && pending.peer != authenticated) {
      enet_peer_disconnect_now(pending.peer, 0);
    }
    pending = {};
  }
}

template <typename T, typename Fn>
void with_goal_buffer(u32 ptr, const char* label, Fn fn) {
  try {
    T* buffer = goal_ptr<T>(ptr);
    if (buffer) {
      fn(buffer);
    }
  } catch (...) {
    lg::error("[Multiplayer] Exception in {}", label);
  }
}
}  // namespace

int pc_multi_get_role() {
  return multiplayer_data().local_role;
}

int pc_multi_set_local_version(u32 version_ptr) {
  auto& data = multiplayer_data();
  data.local_version.clear();
  data.required_version.clear();
  const char* configured_version = goal_string_data(version_ptr);
  if (!configured_version) {
    return 0;
  }
  size_t length = 0;
  while (length <= kMultiplayerVersionMaxLength && configured_version[length] != '\0') {
    ++length;
  }
  if (length > kMultiplayerVersionMaxLength ||
      !mp_resolve_compatibility_identity(std::string_view(configured_version, length),
                                         build_commit(), data.local_version)) {
    data.local_version.clear();
    return 0;
  }
  return 1;
}

u64 pc_multi_get_local_version() {
  return jak2::make_string_from_c(multiplayer_data().local_version.c_str());
}

u64 pc_multi_get_required_version() {
  const auto& data = multiplayer_data();
  if (data.join_status != (int)MultiplayerStatus::VERSION_MISMATCH) {
    return jak2::make_string_from_c("");
  }
  return jak2::make_string_from_c(data.required_version.c_str());
}

void pc_multi_poll(u32 local_ptr, u32 remote_ptr) {
  try {
    auto& data = multiplayer_data();
    if (!data.initialized || !data.host) {
      return;
    }

    static uint32_t last_poll_tick = 0;
    uint32_t current_time = enet_time_get();
    bool is_in_game = data.join_status == (int)MultiplayerStatus::IN_GAME;
    bool is_reconnecting = data.join_status == (int)MultiplayerStatus::RECONNECTING;
    if (!is_in_game && !is_reconnecting && current_time - last_poll_tick < 100) {
      return;
    }
    last_poll_tick = current_time;

    auto* local = goal_ptr<LocalPlayerInfoGOAL>(local_ptr);
    auto* remote = goal_ptr<RemotePlayerInfoGOAL>(remote_ptr);
    if (!local || !remote) {
      return;
    }

    poll_network(data, local, remote);
    current_time = enet_time_get();
    multiplayer_cleanup_stale_sync(data, current_time);
    multiplayer_update_receive_timeout(data, current_time);
  } catch (...) {
    lg::error("[Multiplayer] Exception in pc_multi_poll");
  }
}

void pc_multi_send_state(u32 local_ptr) {
  if (multiplayer_debug_receive_stopped()) {
    return;
  }
  with_goal_buffer<LocalPlayerInfoGOAL>(local_ptr, "pc_multi_send_state", [](auto* local) {
    auto& data = multiplayer_data();
    if (data.initialized && data.host) {
      mp_send_player_state(data, local);
    }
  });
}

void pc_multi_receive_state(u32 remote_ptr) {
  with_goal_buffer<RemotePlayerInfoGOAL>(remote_ptr, "pc_multi_receive_state", [](auto* remote) {
    auto& data = multiplayer_data();
    if (data.initialized && data.host) {
      mp_sync_remote_player_to_goal(data, remote);
    }
  });
}

void pc_multi_send_events(u32 event_ptr) {
  if (multiplayer_debug_receive_stopped()) {
    return;
  }
  with_goal_buffer<MPEventBufferGOAL>(event_ptr, "pc_multi_send_events", [](auto* events) {
    auto& data = multiplayer_data();
    if (data.initialized) {
      mp_send_game_events(data, events);
    }
  });
}

void pc_multi_receive_events(u32 event_ptr) {
  with_goal_buffer<MPEventBufferGOAL>(event_ptr, "pc_multi_receive_events", [](auto* events) {
    auto& data = multiplayer_data();
    if (data.initialized) {
      mp_receive_game_events(data, events);
    }
  });
}

void pc_multi_send_enemies(u32 buffer_ptr) {
  with_goal_buffer<MPEnemySyncBufferGOAL>(buffer_ptr, "pc_multi_send_enemies", [](auto* buffer) {
    auto& data = multiplayer_data();
    if (data.initialized) {
      mp_send_enemy_sync(data, buffer);
    }
  });
}

void pc_multi_receive_enemies(u32 buffer_ptr) {
  with_goal_buffer<MPEnemySyncBufferGOAL>(buffer_ptr, "pc_multi_receive_enemies", [](auto* buffer) {
    auto& data = multiplayer_data();
    if (data.initialized) {
      mp_receive_enemy_sync(data, buffer);
    }
  });
}

void pc_multi_send_traffic(u32 buffer_ptr) {
  with_goal_buffer<MPTrafficSyncBufferGOAL>(buffer_ptr, "pc_multi_send_traffic", [](auto* buffer) {
    auto& data = multiplayer_data();
    if (data.initialized) {
      mp_send_traffic_sync(data, buffer);
    }
  });
}

void pc_multi_receive_traffic(u32 buffer_ptr) {
  with_goal_buffer<MPTrafficSyncBufferGOAL>(buffer_ptr, "pc_multi_receive_traffic",
                                            [](auto* buffer) {
                                              auto& data = multiplayer_data();
                                              if (data.initialized) {
                                                mp_receive_traffic_sync(data, buffer);
                                              }
                                            });
}

void pc_multi_clear_remote_traffic() {
  try {
    multiplayer_reset_remote_traffic_buffers(multiplayer_data());
  } catch (...) {
    lg::error("[Multiplayer] Exception in pc_multi_clear_remote_traffic");
  }
}

void pc_multi_send_palace_squid(u32 buffer_ptr) {
  with_goal_buffer<MPPalaceSquidSyncBufferGOAL>(buffer_ptr, "pc_multi_send_palace_squid",
                                                [](auto* buffer) {
                                                  auto& data = multiplayer_data();
                                                  if (data.initialized) {
                                                    mp_send_palace_squid_sync(data, buffer);
                                                  }
                                                });
}

void pc_multi_receive_palace_squid(u32 buffer_ptr) {
  with_goal_buffer<MPPalaceSquidSyncBufferGOAL>(buffer_ptr, "pc_multi_receive_palace_squid",
                                                [](auto* buffer) {
                                                  auto& data = multiplayer_data();
                                                  if (data.initialized) {
                                                    mp_receive_palace_squid_sync(data, buffer);
                                                  }
                                                });
}

void pc_multi_send_airlock_state(u32 buffer_ptr) {
  if (multiplayer_debug_receive_stopped()) {
    return;
  }
  with_goal_buffer<MPAirlockSyncBufferGOAL>(buffer_ptr, "pc_multi_send_airlock_state",
                                            [](auto* buffer) {
                                              auto& data = multiplayer_data();
                                              if (data.initialized) {
                                                mp_send_airlock_sync(data, buffer);
                                              }
                                            });
}

void pc_multi_receive_airlock_state(u32 buffer_ptr) {
  with_goal_buffer<MPAirlockSyncBufferGOAL>(buffer_ptr, "pc_multi_receive_airlock_state",
                                            [](auto* buffer) {
                                              auto& data = multiplayer_data();
                                              if (data.initialized) {
                                                mp_receive_airlock_sync(data, buffer);
                                              }
                                            });
}

u64 pc_multi_get_enemy_sync_time() {
  return multiplayer_data().last_enemy_sync_time;
}

u64 pc_multi_get_vehicle_sync_time(u32 net_id) {
  if (net_id == 0) {
    return 0;
  }

  const auto& data = multiplayer_data();
  if (data.remote_entity.veh_state.net_id == net_id) {
    return data.remote_entity.receive_tick;
  }

  for (uint32_t slot = 0; slot < MAX_VEHICLE_SYNC_COUNT; ++slot) {
    if (data.traffic_buffer.vehicles[slot].net_id == net_id) {
      return data.veh_last_updated[slot];
    }
  }
  return 0;
}

void pc_multi_disconnect() {
  MultiplayerManager::disconnect(multiplayer_data());
}

void pc_multi_setup_host() {
  MultiplayerManager::setup_host(multiplayer_data(), false);
}

void pc_multi_setup_internet_host() {
  MultiplayerManager::setup_host(multiplayer_data(), true);
}

void pc_multi_setup_client(u32 ip_ptr, u32 port) {
  const char* invite = goal_string_data(ip_ptr);
  if (!invite || !invite[0]) {
    lg::warn("[Multiplayer] Ignoring setup-client with empty invite string.");
    return;
  }
  auto& data = multiplayer_data();
  MultiplayerManager::disconnect(data);
  std::string host;
  uint16_t invite_port = 0;
  if (data.security.start_client(invite, host, invite_port)) {
    MultiplayerManager::setup_client(data, host.c_str(), invite_port);
    return;
  }
  if (port > 0 && port <= (std::numeric_limits<uint16_t>::max)() &&
      MultiplayerScanner::start_direct_search(data, invite, static_cast<uint16_t>(port))) {
    lg::info("[Multiplayer] Retrieving host authentication token.");
    return;
  }
  lg::warn("[Multiplayer] Rejected invalid or legacy multiplayer connection argument.");
  data.join_status = (int)MultiplayerStatus::FAILED;
}

int64_t pc_multi_get_status() {
  return static_cast<int64_t>(MultiplayerScanner::get_status(multiplayer_data()));
}

void pc_multi_set_status(int status) {
  multiplayer_set_status(multiplayer_data(), status);
}

void pc_multi_request_full_sync() {
  auto& data = multiplayer_data();
  if (data.local_role == 0 && data.join_status == (int)MultiplayerStatus::IN_GAME) {
    multiplayer_request_full_sync(data);
    lg::info("[Multiplayer] Full sync requested by GOAL.");
  }
}

void pc_multi_stop_search() {
  MultiplayerScanner::stop_search(multiplayer_data());
}

void pc_multi_start_search() {
  multiplayer_clear_direct_connect_draft(multiplayer_data());
  MultiplayerScanner::start_search(multiplayer_data());
}

u64 pc_multi_get_command_line_arg(u32 str_ptr) {
  const char* arg_name = goal_string_data(str_ptr);
  if (!arg_name) {
    return s7.offset;
  }
  for (int i = 1; i < g_argc; i++) {
    if (g_argv[i] && strcmp(g_argv[i], arg_name) == 0) {
      return jak2::make_string_from_c(i + 1 < g_argc ? g_argv[i + 1] : "");
    }
  }
  return s7.offset;
}

static void connect_private_invite(MultiplayerData& data, std::string& invite) {
  MultiplayerManager::disconnect(data);
  std::string host;
  uint16_t port = 0;
  const bool valid = data.security.start_client(invite, host, port);
  mp_secure_clear_string(invite);
  if (!valid) {
    data.join_status = (int)MultiplayerStatus::FAILED;
    return;
  }
  MultiplayerManager::setup_client(data, host.c_str(), port);
}

void pc_multi_clear_direct_connect() {
  mp_direct_connect_clear(multiplayer_data());
}

void pc_multi_reset_direct_connect() {
  mp_direct_connect_reset(multiplayer_data());
}

u64 pc_multi_get_direct_field(int field) {
  const std::string display = mp_direct_connect_display(multiplayer_data(), field);
  return jak2::make_string_from_c(display.c_str());
}

int pc_multi_edit_direct_field(int field, u32 key) {
  return mp_direct_connect_edit(multiplayer_data(), field, key);
}

int pc_multi_direct_connect_ready() {
  return mp_direct_connect_ready(multiplayer_data()) ? 1 : 0;
}

int pc_multi_connect_direct() {
  return mp_direct_connect_start(multiplayer_data()) ? 1 : 0;
}

void pc_multi_connect_found_host() {
  auto& data = multiplayer_data();
  std::string invite;
  {
    std::lock_guard<std::mutex> lock(data.discovery_result_mutex);
    invite.swap(data.found_ip);
  }
  if (invite.empty()) {
    data.join_status = (int)MultiplayerStatus::FAILED;
    return;
  }
  connect_private_invite(data, invite);
}

static std::string current_host_invite(MultiplayerData& data) {
  std::string address = local_invite_address();
  if (data.internet_host) {
    std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
    if (data.port_mapping_state != MPPortMappingState::READY ||
        data.port_mapping_external_ip.empty()) {
      return {};
    }
    address = data.port_mapping_external_ip;
  }
  return data.security.invite_for_address(address);
}

int pc_multi_get_host_invite_status() {
  return multiplayer_host_invite_status(multiplayer_data());
}

int pc_multi_retry_online_setup() {
  return MultiplayerManager::retry_online_setup(multiplayer_data()) ? 1 : 0;
}

int pc_multi_copy_invite() {
  std::string invite = current_host_invite(multiplayer_data());
  if (invite.empty()) {
    return 0;
  }
  const bool copied = SDL_SetClipboardText(invite.c_str());
  mp_secure_clear_string(invite);
  return copied ? 1 : 0;
}

void pc_multi_clear_staged_invite() {
  auto& data = multiplayer_data();
  mp_secure_clear_string(data.staged_invite);
  data.staged_invite_status = 0;
}

int pc_multi_stage_clipboard_invite() {
  auto& data = multiplayer_data();
  multiplayer_clear_direct_connect_draft(data);
  pc_multi_clear_staged_invite();
  char* clipboard_text = SDL_GetClipboardText();
  if (!clipboard_text) {
    data.staged_invite_status = -1;
    return 0;
  }

  constexpr size_t kMaximumInviteLength = 43;
  size_t length = 0;
  while (length <= kMaximumInviteLength && clipboard_text[length] != '\0') {
    ++length;
  }
  bool valid = length <= kMaximumInviteLength;
  std::string candidate;
  if (valid) {
    candidate.assign(clipboard_text, length);
    MultiplayerSecurity validator;
    std::string host;
    uint16_t port = 0;
    valid = validator.start_client(candidate, host, port);
  }
  SDL_free(clipboard_text);

  if (!valid) {
    mp_secure_clear_string(candidate);
    data.staged_invite_status = -1;
    return 0;
  }
  data.staged_invite.swap(candidate);
  data.staged_invite_status = 1;
  return 1;
}

int pc_multi_get_staged_invite_status() {
  return multiplayer_data().staged_invite_status;
}

void pc_multi_connect_staged_invite() {
  auto& data = multiplayer_data();
  if (data.staged_invite_status != 1 || data.staged_invite.empty()) {
    data.join_status = (int)MultiplayerStatus::FAILED;
    return;
  }

  std::string invite;
  invite.swap(data.staged_invite);
  data.staged_invite_status = 0;
  connect_private_invite(data, invite);
}

void pc_multi_debug_stop_receive(u32 val) {
  multiplayer_set_debug_receive_stopped(val != 0);
}

u64 pc_multi_get_ticks() {
  return enet_time_get();
}

int pc_multi_get_ping() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  if (data.server_peer) {
    return data.server_peer->roundTripTime;
  }

  u32 total = 0;
  u32 count = 0;
  for (size_t i = 0; i < data.host->peerCount; i++) {
    if (data.host->peers[i].state == ENET_PEER_STATE_CONNECTED) {
      total += data.host->peers[i].roundTripTime;
      count++;
    }
  }
  return count > 0 ? (total / count) : 0;
}

int pc_multi_get_packet_loss() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  if (data.server_peer) {
    return (data.server_peer->packetLoss * 100) / 65536;
  }

  u32 total = 0;
  u32 count = 0;
  for (size_t i = 0; i < data.host->peerCount; i++) {
    if (data.host->peers[i].state == ENET_PEER_STATE_CONNECTED) {
      total += (data.host->peers[i].packetLoss * 100) / 65536;
      count++;
    }
  }
  return count > 0 ? (total / count) : 0;
}

int pc_multi_get_ping_variance() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  if (data.server_peer) {
    return data.server_peer->roundTripTimeVariance;
  }

  u32 total = 0;
  u32 count = 0;
  for (size_t i = 0; i < data.host->peerCount; i++) {
    if (data.host->peers[i].state == ENET_PEER_STATE_CONNECTED) {
      total += data.host->peers[i].roundTripTimeVariance;
      count++;
    }
  }
  return count > 0 ? (total / count) : 0;
}

int pc_multi_get_total_sent_bytes() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  return data.host->totalSentData;
}

int pc_multi_get_total_received_bytes() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  return data.host->totalReceivedData;
}

int pc_multi_get_total_sent_packets() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  return data.host->totalSentPackets;
}

int pc_multi_get_total_received_packets() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  return data.host->totalReceivedPackets;
}

int pc_multi_get_send_rate() {
  auto& data = multiplayer_data();
  return data.stats.send_rate_bytes_per_sec;
}

int pc_multi_get_recv_rate() {
  auto& data = multiplayer_data();
  return data.stats.recv_rate_bytes_per_sec;
}

int pc_multi_get_type_send_rate(int type) {
  if (type < 0 || type >= 11)
    return 0;
  return multiplayer_data().stats.send_rate_by_type[type];
}

int pc_multi_get_type_recv_rate(int type) {
  if (type < 0 || type >= 11)
    return 0;
  return multiplayer_data().stats.recv_rate_by_type[type];
}

int pc_multi_get_type_total_sent(int type) {
  if (type < 0 || type >= 11)
    return 0;
  return (int)multiplayer_data().stats.sent_bytes_by_type[type];
}

int pc_multi_get_type_total_recv(int type) {
  if (type < 0 || type >= 11)
    return 0;
  return (int)multiplayer_data().stats.recv_bytes_by_type[type];
}

void init_multiplayer_pc_port() {
  jak2::make_function_symbol_from_c("pc-multi-set-local-version",
                                    (void*)pc_multi_set_local_version);
  jak2::make_function_symbol_from_c("pc-multi-get-local-version",
                                    (void*)pc_multi_get_local_version);
  jak2::make_function_symbol_from_c("pc-multi-get-required-version",
                                    (void*)pc_multi_get_required_version);
  jak2::make_function_symbol_from_c("pc-multi-setup-host", (void*)pc_multi_setup_host);
  jak2::make_function_symbol_from_c("pc-multi-setup-internet-host",
                                    (void*)pc_multi_setup_internet_host);
  jak2::make_function_symbol_from_c("pc-multi-setup-client", (void*)pc_multi_setup_client);
  jak2::make_function_symbol_from_c("pc-multi-get-status", (void*)pc_multi_get_status);
  jak2::make_function_symbol_from_c("pc-multi-set-status", (void*)pc_multi_set_status);
  jak2::make_function_symbol_from_c("pc-multi-request-full-sync",
                                    (void*)pc_multi_request_full_sync);
  jak2::make_function_symbol_from_c("pc-multi-stop-search", (void*)pc_multi_stop_search);
  jak2::make_function_symbol_from_c("pc-multi-start-search", (void*)pc_multi_start_search);
  jak2::make_function_symbol_from_c("pc-multi-connect-found-host",
                                    (void*)pc_multi_connect_found_host);
  jak2::make_function_symbol_from_c("pc-multi-get-host-invite-status",
                                    (void*)pc_multi_get_host_invite_status);
  jak2::make_function_symbol_from_c("pc-multi-retry-online-setup",
                                    (void*)pc_multi_retry_online_setup);
  jak2::make_function_symbol_from_c("pc-multi-copy-invite", (void*)pc_multi_copy_invite);
  jak2::make_function_symbol_from_c("pc-multi-stage-clipboard-invite",
                                    (void*)pc_multi_stage_clipboard_invite);
  jak2::make_function_symbol_from_c("pc-multi-get-staged-invite-status",
                                    (void*)pc_multi_get_staged_invite_status);
  jak2::make_function_symbol_from_c("pc-multi-connect-staged-invite",
                                    (void*)pc_multi_connect_staged_invite);
  jak2::make_function_symbol_from_c("pc-multi-clear-staged-invite",
                                    (void*)pc_multi_clear_staged_invite);
  jak2::make_function_symbol_from_c("pc-multi-clear-direct-connect",
                                    (void*)pc_multi_clear_direct_connect);
  jak2::make_function_symbol_from_c("pc-multi-reset-direct-connect",
                                    (void*)pc_multi_reset_direct_connect);
  jak2::make_function_symbol_from_c("pc-multi-get-direct-field",
                                    (void*)pc_multi_get_direct_field);
  jak2::make_function_symbol_from_c("pc-multi-edit-direct-field",
                                    (void*)pc_multi_edit_direct_field);
  jak2::make_function_symbol_from_c("pc-multi-direct-connect-ready",
                                    (void*)pc_multi_direct_connect_ready);
  jak2::make_function_symbol_from_c("pc-multi-connect-direct",
                                    (void*)pc_multi_connect_direct);
  jak2::make_function_symbol_from_c("pc-multi-poll", (void*)pc_multi_poll);
  jak2::make_function_symbol_from_c("pc-multi-send-state", (void*)pc_multi_send_state);
  jak2::make_function_symbol_from_c("pc-multi-receive-state", (void*)pc_multi_receive_state);
  jak2::make_function_symbol_from_c("pc-multi-send-events", (void*)pc_multi_send_events);
  jak2::make_function_symbol_from_c("pc-multi-receive-events", (void*)pc_multi_receive_events);
  jak2::make_function_symbol_from_c("pc-multi-send-enemies", (void*)pc_multi_send_enemies);
  jak2::make_function_symbol_from_c("pc-multi-receive-enemies", (void*)pc_multi_receive_enemies);
  jak2::make_function_symbol_from_c("pc-multi-send-traffic", (void*)pc_multi_send_traffic);
  jak2::make_function_symbol_from_c("pc-multi-receive-traffic", (void*)pc_multi_receive_traffic);
  jak2::make_function_symbol_from_c("pc-multi-clear-remote-traffic",
                                    (void*)pc_multi_clear_remote_traffic);
  jak2::make_function_symbol_from_c("pc-multi-send-palace-squid",
                                    (void*)pc_multi_send_palace_squid);
  jak2::make_function_symbol_from_c("pc-multi-receive-palace-squid",
                                    (void*)pc_multi_receive_palace_squid);
  jak2::make_function_symbol_from_c("pc-multi-send-airlock-state",
                                    (void*)pc_multi_send_airlock_state);
  jak2::make_function_symbol_from_c("pc-multi-receive-airlock-state",
                                    (void*)pc_multi_receive_airlock_state);
  jak2::make_function_symbol_from_c("pc-multi-get-enemy-sync-time",
                                    (void*)pc_multi_get_enemy_sync_time);
  jak2::make_function_symbol_from_c("pc-multi-get-vehicle-sync-time",
                                    (void*)pc_multi_get_vehicle_sync_time);
  jak2::make_function_symbol_from_c("pc-multi-get-role", (void*)pc_multi_get_role);
  jak2::make_function_symbol_from_c("pc-multi-disconnect", (void*)pc_multi_disconnect);
  jak2::make_function_symbol_from_c("pc-multi-get-command-line-arg",
                                    (void*)pc_multi_get_command_line_arg);
  jak2::make_function_symbol_from_c("pc-multi-debug-stop-receive",
                                    (void*)pc_multi_debug_stop_receive);
  jak2::make_function_symbol_from_c("pc-multi-get-ticks", (void*)pc_multi_get_ticks);
  jak2::make_function_symbol_from_c("pc-multi-get-ping", (void*)pc_multi_get_ping);
  jak2::make_function_symbol_from_c("pc-multi-get-packet-loss", (void*)pc_multi_get_packet_loss);
  jak2::make_function_symbol_from_c("pc-multi-get-ping-variance",
                                    (void*)pc_multi_get_ping_variance);
  jak2::make_function_symbol_from_c("pc-multi-get-total-sent-bytes",
                                    (void*)pc_multi_get_total_sent_bytes);
  jak2::make_function_symbol_from_c("pc-multi-get-total-received-bytes",
                                    (void*)pc_multi_get_total_received_bytes);
  jak2::make_function_symbol_from_c("pc-multi-get-total-sent-packets",
                                    (void*)pc_multi_get_total_sent_packets);
  jak2::make_function_symbol_from_c("pc-multi-get-total-received-packets",
                                    (void*)pc_multi_get_total_received_packets);
  jak2::make_function_symbol_from_c("pc-multi-get-send-rate", (void*)pc_multi_get_send_rate);
  jak2::make_function_symbol_from_c("pc-multi-get-recv-rate", (void*)pc_multi_get_recv_rate);
  jak2::make_function_symbol_from_c("pc-multi-get-type-send-rate",
                                    (void*)pc_multi_get_type_send_rate);
  jak2::make_function_symbol_from_c("pc-multi-get-type-recv-rate",
                                    (void*)pc_multi_get_type_recv_rate);
  jak2::make_function_symbol_from_c("pc-multi-get-type-total-sent",
                                    (void*)pc_multi_get_type_total_sent);
  jak2::make_function_symbol_from_c("pc-multi-get-type-total-recv",
                                    (void*)pc_multi_get_type_total_recv);
}
