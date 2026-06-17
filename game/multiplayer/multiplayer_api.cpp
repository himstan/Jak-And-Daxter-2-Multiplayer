#include "multiplayer_api.h"

#include "common/log/log.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/multiplayer/multiplayer.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_port_mapping.h"
#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/multiplayer_scanner.h"
#include "game/multiplayer/multiplayer_session.h"
#include "game/multiplayer/multiplayer_types.h"
#include "game/multiplayer/pedestrian/multiplayer_pedestrian.h"
#include "game/multiplayer/sync/enemy_sync.h"
#include "game/multiplayer/sync/event_sync.h"
#include "game/multiplayer/sync/palace_squid_sync.h"
#include "game/multiplayer/sync/player_sync.h"
#include "game/multiplayer/sync/traffic_sync.h"
#include "game/multiplayer/vehicle/multiplayer_vehicle.h"

#include "enet/enet.h"

#include <cstring>

namespace {
constexpr u32 kMinGoalPointer = 0x1000;
constexpr uint32_t kPortMappingRefreshIntervalMs = 60 * 60 * 1000;

template <typename T>
T* goal_ptr(u32 ptr) {
  if (ptr < kMinGoalPointer) {
    return nullptr;
  }
  return Ptr<T>(ptr).c();
}

const char* goal_string_data(u32 ptr) {
  if (ptr < kMinGoalPointer) {
    return nullptr;
  }
  auto* str = Ptr<String>(ptr).c();
  if (!str) {
    return nullptr;
  }
  return str->data();
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

  switch (view.type()) {
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
    case PacketType::FULL_SYNC:
      mp_handle_full_sync_packet(packet, local, remote);
      break;
    case PacketType::EVENT_JOIN:
    case PacketType::EVENT_LEAVE:
      break;
    default:
      if (current_time - data.last_traffic_short_packet_debug_time > 2000) {
        lg::warn("[Multiplayer] Ignoring unknown packet type {} ({} bytes).",
                 (int)view.type(), view.size());
        data.last_traffic_short_packet_debug_time = current_time;
      }
      break;
  }
}

void poll_network(MultiplayerData& data, LocalPlayerInfoGOAL* local, RemotePlayerInfoGOAL* remote) {
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
      case ENET_EVENT_TYPE_RECEIVE:
        data.last_receive_time = current_time;
        if (data.join_status == (int)MultiplayerStatus::RECONNECTING) {
          lg::info("[Multiplayer] Data packet received. Connection restored. Resuming status {}...",
                   data.pre_reconnect_status);
          data.join_status = data.pre_reconnect_status;
        }
        handle_receive_packet(data, event.packet, local, remote, current_time);
        enet_packet_destroy(event.packet);
        break;
      case ENET_EVENT_TYPE_CONNECT:
        data.last_receive_time = current_time;
        if (data.local_role == 1) {
          lg::info("[Multiplayer] Successfully connected to host.");
          data.join_status = (int)MultiplayerStatus::CONNECTED_LOBBY;
        } else if (data.local_role == 0) {
          char ip[64];
          enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip));
          lg::info("[Multiplayer] Client connected");
          if (data.join_status != (int)MultiplayerStatus::IN_GAME) {
            data.join_status = (int)MultiplayerStatus::CONNECTED_LOBBY;
          } else {
            multiplayer_request_full_sync(data);
          }
        }
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        if (data.local_role == 0) {
          lg::info("[Multiplayer] Client disconnected.");
          data.remote_entities.erase(1);
        } else {
          lg::warn("[Multiplayer] Host has left the session.");
          data.join_status = (int)MultiplayerStatus::HOST_LEFT;
        }
        break;
      default:
        break;
    }
  }
}

void refresh_host_port_mapping(MultiplayerData& data, uint32_t current_time) {
  MPPortMappingMethod method = MPPortMappingMethod::NONE;
  uint16_t local_port = 0;
  uint16_t external_port = 0;
  {
    std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
    if (data.local_role != 0 || !data.port_mapping_active ||
        data.port_mapping_method != MPPortMappingMethod::NAT_PMP ||
        current_time - data.last_port_mapping_refresh_time < kPortMappingRefreshIntervalMs) {
      return;
    }
    method = data.port_mapping_method;
    local_port = data.port_mapping_local_port;
    external_port = data.port_mapping_external_port;
  }

  if (mp_refresh_udp_port_mapping(method, local_port, external_port)) {
    std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
    data.last_port_mapping_refresh_time = current_time;
  } else {
    std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
    data.last_port_mapping_refresh_time = current_time;
    lg::warn("[Multiplayer] Temporary UDP port mapping refresh failed.");
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
}

int pc_multi_get_role() {
  return multiplayer_data().local_role;
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
    refresh_host_port_mapping(data, current_time);
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
  with_goal_buffer<MPTrafficSyncBufferGOAL>(buffer_ptr, "pc_multi_receive_traffic", [](auto* buffer) {
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
  with_goal_buffer<MPPalaceSquidSyncBufferGOAL>(
      buffer_ptr, "pc_multi_send_palace_squid", [](auto* buffer) {
        auto& data = multiplayer_data();
        if (data.initialized) {
          mp_send_palace_squid_sync(data, buffer);
        }
      });
}

void pc_multi_receive_palace_squid(u32 buffer_ptr) {
  with_goal_buffer<MPPalaceSquidSyncBufferGOAL>(
      buffer_ptr, "pc_multi_receive_palace_squid", [](auto* buffer) {
        auto& data = multiplayer_data();
        if (data.initialized) {
          mp_receive_palace_squid_sync(data, buffer);
        }
      });
}

u64 pc_multi_get_enemy_sync_time() {
  return multiplayer_data().last_enemy_sync_time;
}

void pc_multi_disconnect() {
  MultiplayerManager::disconnect(multiplayer_data());
}

void pc_multi_setup_host() {
  MultiplayerManager::setup_host(multiplayer_data());
}

void pc_multi_setup_client(u32 ip_ptr, u32 port) {
  const char* ip = goal_string_data(ip_ptr);
  if (!ip || !ip[0]) {
    lg::warn("[Multiplayer] Ignoring setup-client with invalid IP string.");
    return;
  }
  MultiplayerManager::setup_client(multiplayer_data(), ip, (int)port);
}

int pc_multi_get_status() {
  return MultiplayerScanner::get_status(multiplayer_data());
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

u64 pc_multi_get_found_ip() {
  return jak2::make_string_from_c(multiplayer_data().found_ip.c_str());
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

void init_multiplayer_pc_port() {
  jak2::make_function_symbol_from_c("pc-multi-setup-host", (void*)pc_multi_setup_host);
  jak2::make_function_symbol_from_c("pc-multi-setup-client", (void*)pc_multi_setup_client);
  jak2::make_function_symbol_from_c("pc-multi-get-status", (void*)pc_multi_get_status);
  jak2::make_function_symbol_from_c("pc-multi-set-status", (void*)pc_multi_set_status);
  jak2::make_function_symbol_from_c("pc-multi-request-full-sync", (void*)pc_multi_request_full_sync);
  jak2::make_function_symbol_from_c("pc-multi-stop-search", (void*)pc_multi_stop_search);
  jak2::make_function_symbol_from_c("pc-multi-start-search", (void*)pc_multi_start_search);
  jak2::make_function_symbol_from_c("pc-multi-get-found-ip", (void*)pc_multi_get_found_ip);
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
  jak2::make_function_symbol_from_c("pc-multi-send-palace-squid", (void*)pc_multi_send_palace_squid);
  jak2::make_function_symbol_from_c("pc-multi-receive-palace-squid",
                                    (void*)pc_multi_receive_palace_squid);
  jak2::make_function_symbol_from_c("pc-multi-get-enemy-sync-time",
                                    (void*)pc_multi_get_enemy_sync_time);
  jak2::make_function_symbol_from_c("pc-multi-get-role", (void*)pc_multi_get_role);
  jak2::make_function_symbol_from_c("pc-multi-disconnect", (void*)pc_multi_disconnect);
  jak2::make_function_symbol_from_c("pc-multi-get-command-line-arg",
                                    (void*)pc_multi_get_command_line_arg);
  jak2::make_function_symbol_from_c("pc-multi-debug-stop-receive",
                                    (void*)pc_multi_debug_stop_receive);
  jak2::make_function_symbol_from_c("pc-multi-get-ticks", (void*)pc_multi_get_ticks);
  jak2::make_function_symbol_from_c("pc-multi-get-ping", (void*)pc_multi_get_ping);
  jak2::make_function_symbol_from_c("pc-multi-get-packet-loss", (void*)pc_multi_get_packet_loss);
}
