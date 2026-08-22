#include "multiplayer_session.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <sodium.h>
#include <string>

#include "common/log/log.h"

#include "enet/enet.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_peer_registry.h"
#include "game/multiplayer/multiplayer_protocol.h"

namespace {
MultiplayerData g_multiplayer_data;
bool g_debug_receive_stopped = false;

constexpr uint32_t kReceiveTimeoutMilliseconds = 10000;
constexpr uint32_t kReconnectDelayMilliseconds[] = {250, 500, 1000, 2000, 5000};

std::string enet_peer_endpoint_string(const ENetPeer* peer) {
  if (!peer) {
    return "<none>";
  }
  char host[64] = {};
  if (enet_address_get_host_ip(&peer->address, host, sizeof(host)) != 0) {
    return "<unknown>:" + std::to_string(peer->address.port);
  }
  return std::string(host) + ":" + std::to_string(peer->address.port);
}

uint32_t reconnect_delay_for_attempt(uint32_t attempt_count) {
  constexpr uint32_t kMaximumDelayIndex =
      static_cast<uint32_t>(std::size(kReconnectDelayMilliseconds) - 1);
  const uint32_t delay_index = (std::min)(attempt_count, kMaximumDelayIndex);
  return kReconnectDelayMilliseconds[delay_index];
}

bool time_reached(uint32_t current_time, uint32_t deadline) {
  return static_cast<int32_t>(current_time - deadline) >= 0;
}

void clear_reconnect_tracking(MultiplayerData& data) {
  data.reconnect_attempt_active = false;
  data.reconnect_waiting_for_bootstrap = false;
  data.reconnect_attempt_count = 0;
  data.reconnect_next_attempt_time = 0;
}
}  // namespace

MultiplayerData& multiplayer_data() {
  return g_multiplayer_data;
}

bool multiplayer_debug_receive_stopped() {
  return g_debug_receive_stopped;
}

void multiplayer_set_debug_receive_stopped(bool stopped) {
  g_debug_receive_stopped = stopped;
}

void multiplayer_reset_remote_traffic_buffers(MultiplayerData& data) {
  memset(&data.traffic_buffer, 0, sizeof(data.traffic_buffer));
  memset(data.ped_last_updated, 0, sizeof(data.ped_last_updated));
  memset(data.veh_last_updated, 0, sizeof(data.veh_last_updated));
  memset(data.veh_last_sequence, 0, sizeof(data.veh_last_sequence));
  data.last_pedestrian_sequence_by_source = {};
  data.last_vehicle_sequence_by_source = {};
  data.remote_traffic_buffer_level_hash = 0;
  data.last_traffic_sync_time = 0;
}

void multiplayer_reset_remote_palace_squid_state(MultiplayerData& data) {
  memset(&data.remote_palace_squid_state, 0, sizeof(data.remote_palace_squid_state));
  data.last_palace_squid_sync_time = 0;
}

void multiplayer_reset_remote_airlock_state(MultiplayerData& data) {
  memset(&data.remote_airlock_table, 0, sizeof(data.remote_airlock_table));
  data.last_airlock_sync_time = 0;
  data.last_airlock_sequence_by_player = {};
}

void multiplayer_clear_remote_peer_state(MultiplayerData& data) {
  data.packet_scheduler.clear();
  data.local_join_identity_sent = false;
  data.server_last_receive_time = 0;
  data.inbound_events.clear();
  data.last_event_sequence_by_player = {};
  data.player_states = {};
  data.last_world_sequence = 0;
  memset(&data.remote_enemy_buffer, 0, sizeof(data.remote_enemy_buffer));
  data.last_enemy_sync_time = 0;
  data.last_enemy_sequence_by_player = {};
  data.traffic_authority_map.fill(kMPInvalidCompactPlayerId);
  data.traffic_authority_revision = 0;
  data.selected_traffic_authority = kMPInvalidCompactPlayerId;
  data.last_remote_traffic_level_hash = 0;
  multiplayer_reset_remote_traffic_buffers(data);
  multiplayer_reset_remote_palace_squid_state(data);
  multiplayer_reset_remote_airlock_state(data);
}

void multiplayer_clear_direct_connect_draft(MultiplayerData& data) {
  sodium_memzero(data.direct_address.data(), data.direct_address.size());
  sodium_memzero(data.direct_port.data(), data.direct_port.size());
  sodium_memzero(data.direct_room_code.data(), data.direct_room_code.size());
}

bool multiplayer_handle_host_leave(MultiplayerData& data,
                                   ENetPeer* sender,
                                   MultiplayerLeaveReason reason) {
  if (data.session_role != 1 || !sender || sender != data.server_peer ||
      !data.security.authenticated() || reason != MultiplayerLeaveReason::HOST_CLOSED) {
    return false;
  }

  if (sender->state != ENET_PEER_STATE_DISCONNECTED) {
    enet_peer_disconnect_now(sender, kDisconnectReasonHostClosed);
  }
  multiplayer_clear_remote_peer_state(data);
  multiplayer_cancel_client_reconnect(data);
  data.client_handshake_started_time = 0;
  data.server_peer = nullptr;
  data.security.reset();
  data.join_status = (int)MultiplayerStatus::HOST_LEFT;
  lg::warn("[Multiplayer] Host closed the session.");
  return true;
}

void multiplayer_clear_session_state(MultiplayerData& data, bool preserve_reconnect_state) {
  std::string saved_reconnect_invite;
  if (preserve_reconnect_state) {
    saved_reconnect_invite.swap(data.reconnect_invite);
  }

  multiplayer_clear_remote_peer_state(data);
  multiplayer_host_peer_reset_all(data);
  data.stats.reset();
  data.host_game_active = false;
  data.lobby_countdown_active = false;
  data.lobby_countdown_target_time_ms = 0;
  data.server_peer = nullptr;
  mp_secure_clear_string(data.staged_invite);
  data.staged_invite_status = 0;
  if (preserve_reconnect_state) {
    data.reconnect_invite.swap(saved_reconnect_invite);
  } else {
    mp_secure_clear_string(data.reconnect_invite);
    multiplayer_cancel_client_reconnect(data);
  }
  multiplayer_clear_direct_connect_draft(data);
  data.required_version.clear();
  data.local_traffic_level_hash = 0;
  data.traffic_authority_map.fill(kMPInvalidCompactPlayerId);
  data.traffic_authority_revision = 0;
  data.selected_traffic_authority = kMPInvalidCompactPlayerId;
  {
    std::lock_guard<std::mutex> lock(data.discovery_result_mutex);
    std::fill(data.found_ip.begin(), data.found_ip.end(), '\0');
    data.found_ip.clear();
    data.directed_discovery = false;
    data.directed_discovery_address = 0;
    data.directed_discovery_game_port = 0;
  }
}

void multiplayer_request_bootstrap(MultiplayerData& data) {
  if (data.session_role == 0) {
    multiplayer_host_request_bootstrap_for_all(data);
  }
}

void multiplayer_set_status(MultiplayerData& data, int status) {
  int old_status = data.join_status;
  data.join_status = status;
  if (old_status != status) {
    lg::info("[Multiplayer] Status transition: {} -> {}", old_status, status);
  }
  if (data.session_role == 0 && status == (int)MultiplayerStatus::IN_GAME &&
      old_status != (int)MultiplayerStatus::IN_GAME) {
    data.host_game_active = true;
    multiplayer_request_bootstrap(data);
  }
  if (data.session_role == 1 && status == (int)MultiplayerStatus::IN_GAME &&
      data.reconnect_waiting_for_bootstrap) {
    multiplayer_note_client_reconnect_completed(data);
  }
}

void multiplayer_enter_client_reconnect(MultiplayerData& data, uint32_t current_time) {
  if (data.join_status == (int)MultiplayerStatus::RECONNECTING) {
    return;
  }

  data.join_status = (int)MultiplayerStatus::RECONNECTING;
  data.reconnect_attempt_active = false;
  data.reconnect_waiting_for_bootstrap = false;
  data.reconnect_next_attempt_time =
      current_time + reconnect_delay_for_attempt(data.reconnect_attempt_count);
  lg::warn("[Multiplayer] Client transport lost; reconnect scheduled in {} ms.",
           reconnect_delay_for_attempt(data.reconnect_attempt_count));
}

bool multiplayer_client_reconnect_due(const MultiplayerData& data, uint32_t current_time) {
  return data.session_role == 1 && data.join_status == (int)MultiplayerStatus::RECONNECTING &&
         !data.reconnect_attempt_active && data.reconnect_next_attempt_time != 0 &&
         time_reached(current_time, data.reconnect_next_attempt_time);
}

void multiplayer_note_client_reconnect_attempt_started(MultiplayerData& data) {
  data.reconnect_attempt_active = true;
  data.reconnect_waiting_for_bootstrap = false;
  data.reconnect_next_attempt_time = 0;
}

void multiplayer_note_client_reconnect_failed(MultiplayerData& data, uint32_t current_time) {
  data.reconnect_attempt_active = false;
  data.reconnect_waiting_for_bootstrap = false;
  if (data.reconnect_attempt_count < 4) {
    ++data.reconnect_attempt_count;
  }
  data.join_status = (int)MultiplayerStatus::RECONNECTING;
  const uint32_t delay = reconnect_delay_for_attempt(data.reconnect_attempt_count);
  data.reconnect_next_attempt_time = current_time + delay;
  lg::warn("[Multiplayer] Reconnect attempt failed; retrying in {} ms.", delay);
}

void multiplayer_note_client_reconnect_authenticated(MultiplayerData& data) {
  const bool reconnecting = data.reconnect_attempt_active || data.reconnect_waiting_for_bootstrap;
  data.reconnect_attempt_active = false;
  data.reconnect_waiting_for_bootstrap = reconnecting;
  if (!reconnecting) {
    data.reconnect_attempt_count = 0;
  }
  data.reconnect_next_attempt_time = 0;
}

void multiplayer_note_client_reconnect_completed(MultiplayerData& data) {
  clear_reconnect_tracking(data);
}

void multiplayer_cancel_client_reconnect(MultiplayerData& data) {
  clear_reconnect_tracking(data);
}

void multiplayer_handle_client_handshake_timeout(MultiplayerData& data, uint32_t current_time) {
  lg::warn(
      "[MP-Handshake] Handling client handshake timeout (peer={}, now={}, status={}, "
      "reconnect_active={}, attempt_count={}, invite_saved={}).",
      enet_peer_endpoint_string(data.server_peer), current_time, data.join_status.load(),
      data.reconnect_attempt_active, data.reconnect_attempt_count, !data.reconnect_invite.empty());
  if (data.reconnect_attempt_active || data.join_status == (int)MultiplayerStatus::RECONNECTING) {
    MultiplayerManager::disconnect(data, true);
    multiplayer_note_client_reconnect_failed(data, current_time);
  } else {
    data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::HOST_UNREACHABLE);
    MultiplayerManager::disconnect(data);
  }
}

void multiplayer_cleanup_stale_sync(MultiplayerData& data, uint32_t current_time) {
  for (uint32_t i = 0; i < MAX_ENEMY_SYNC_COUNT; i++) {
    auto& enemy = data.remote_enemy_buffer.remote_enemies[i];
    if (enemy.actor_id != 0 && current_time - enemy.last_updated > 2000) {
      enemy.actor_id = 0;
    }
  }
  for (uint32_t i = 0; i < MAX_PEDESTRIAN_SYNC_COUNT; i++) {
    if (data.traffic_buffer.pedestrians[i].net_id != 0 &&
        current_time - data.ped_last_updated[i] > 2000) {
      data.traffic_buffer.pedestrians[i].net_id = 0;
    }
  }
  for (uint32_t i = 0; i < MAX_VEHICLE_SYNC_COUNT; i++) {
    if (data.traffic_buffer.vehicles[i].net_id != 0 &&
        current_time - data.veh_last_updated[i] > 2000) {
      data.traffic_buffer.vehicles[i].net_id = 0;
      data.veh_last_updated[i] = 0;
      data.veh_last_sequence[i] = 0;
    }
  }
}

void multiplayer_update_receive_timeout(MultiplayerData& data, uint32_t current_time) {
  const bool has_authenticated_peer = data.session_role == 1 && data.server_peer &&
                                      data.server_peer->state != ENET_PEER_STATE_DISCONNECTED &&
                                      data.security.authenticated();
  const bool client_waiting_for_bootstrap =
      data.session_role == 1 && (data.join_status == (int)MultiplayerStatus::CONNECTED_LOBBY ||
                                 data.join_status == (int)MultiplayerStatus::GAME_STARTING);
  const bool should_check_timeout =
      data.join_status == (int)MultiplayerStatus::IN_GAME || client_waiting_for_bootstrap;
  if (should_check_timeout && has_authenticated_peer && data.server_last_receive_time != 0 &&
      current_time - data.server_last_receive_time > kReceiveTimeoutMilliseconds) {
    multiplayer_enter_client_reconnect(data, current_time);
  }
}
