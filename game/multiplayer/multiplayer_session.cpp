#include "multiplayer_session.h"

#include <algorithm>
#include <cstring>
#include <sodium.h>

#include "common/log/log.h"

#include "enet/enet.h"
#include "game/multiplayer/multiplayer_protocol.h"

namespace {
MultiplayerData g_multiplayer_data;
bool g_debug_receive_stopped = false;
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
  data.remote_traffic_buffer_level_hash = 0;
  data.last_traffic_sync_time = 0;
  data.last_pedestrian_sequence = 0;
}

void multiplayer_reset_remote_palace_squid_state(MultiplayerData& data) {
  memset(&data.remote_palace_squid_state, 0, sizeof(data.remote_palace_squid_state));
  data.last_palace_squid_sync_time = 0;
}

void multiplayer_reset_remote_airlock_state(MultiplayerData& data) {
  memset(&data.remote_airlock_table, 0, sizeof(data.remote_airlock_table));
  data.last_airlock_sync_time = 0;
  data.last_remote_airlock_sequence = 0;
}

void multiplayer_clear_remote_peer_state(MultiplayerData& data) {
  data.packet_scheduler.clear();
  data.pending_full_sync = false;
  data.pending_full_sync_sent_once = false;
  data.last_full_sync_send_time = 0;
  data.last_receive_time = 0;
  data.pre_reconnect_status = 0;
  data.authenticated_peer = nullptr;
  data.inbound_events.clear();
  data.remote_entity = {};
  memset(&data.remote_enemy_buffer, 0, sizeof(data.remote_enemy_buffer));
  data.last_enemy_sync_time = 0;
  data.last_enemy_sequence = 0;
  data.last_remote_traffic_level_hash = 0;
  multiplayer_reset_remote_traffic_buffers(data);
  multiplayer_reset_remote_palace_squid_state(data);
  multiplayer_reset_remote_airlock_state(data);
}

void multiplayer_clear_direct_connect_draft(MultiplayerData& data) {
  sodium_memzero(data.direct_address.data(), data.direct_address.size());
  sodium_memzero(data.direct_port.data(), data.direct_port.size());
  sodium_memzero(data.direct_token.data(), data.direct_token.size());
}

bool multiplayer_prepare_host_for_next_peer(MultiplayerData& data) {
  multiplayer_clear_remote_peer_state(data);
  data.pending_handshakes = {};
  if (!data.security.rotate_host_peer_session()) {
    data.join_status = (int)MultiplayerStatus::FAILED;
    return false;
  }
  data.join_status =
      data.host_game_active ? (int)MultiplayerStatus::IN_GAME : (int)MultiplayerStatus::CONNECTING;
  return true;
}

void multiplayer_clear_session_state(MultiplayerData& data) {
  multiplayer_clear_remote_peer_state(data);
  data.stats.reset();
  data.host_game_active = false;
  data.server_peer = nullptr;
  mp_secure_clear_string(data.staged_invite);
  data.staged_invite_status = 0;
  mp_secure_clear_string(data.reconnect_invite);
  multiplayer_clear_direct_connect_draft(data);
  data.required_version.clear();
  data.local_traffic_level_hash = 0;
  {
    std::lock_guard<std::mutex> lock(data.discovery_result_mutex);
    std::fill(data.found_ip.begin(), data.found_ip.end(), '\0');
    data.found_ip.clear();
    data.directed_discovery = false;
    data.directed_discovery_address = 0;
    data.directed_discovery_game_port = 0;
  }
}

void multiplayer_request_full_sync(MultiplayerData& data) {
  data.pending_full_sync = true;
  data.pending_full_sync_sent_once = false;
  data.last_full_sync_send_time = 0;
}

void multiplayer_set_status(MultiplayerData& data, int status) {
  int old_status = data.join_status;
  data.join_status = status;
  if (old_status != status) {
    lg::info("[Multiplayer] Status transition: {} -> {}", old_status, status);
  }
  if (data.local_role == 0 && status == (int)MultiplayerStatus::IN_GAME &&
      old_status != (int)MultiplayerStatus::IN_GAME) {
    data.host_game_active = true;
    multiplayer_request_full_sync(data);
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
  bool host_has_peer = data.host && data.host->connectedPeers > 0;
  bool should_check_timeout = data.local_role == 1 || (data.local_role == 0 && host_has_peer);
  if (should_check_timeout && data.join_status == (int)MultiplayerStatus::IN_GAME &&
      data.last_receive_time != 0 && current_time - data.last_receive_time > 10000) {
    data.pre_reconnect_status = data.join_status;
    data.join_status = (int)MultiplayerStatus::RECONNECTING;
  }
}
