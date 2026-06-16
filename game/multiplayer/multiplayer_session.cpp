#include "multiplayer_session.h"

#include "common/log/log.h"
#include "game/multiplayer/multiplayer_protocol.h"

#include "enet/enet.h"

#include <cstring>

namespace {
MultiplayerData g_multiplayer_data;
bool g_debug_receive_stopped = false;
}

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
  data.remote_traffic_buffer_level_hash = 0;
  data.last_traffic_sync_time = 0;
}

void multiplayer_reset_remote_palace_squid_state(MultiplayerData& data) {
  memset(&data.remote_palace_squid_state, 0, sizeof(data.remote_palace_squid_state));
  data.last_palace_squid_sync_time = 0;
}

void multiplayer_clear_session_state(MultiplayerData& data) {
  data.pending_full_sync = false;
  data.pending_full_sync_sent_once = false;
  data.last_full_sync_send_time = 0;
  data.last_receive_time = 0;
  data.pre_reconnect_status = 0;
  data.server_peer = nullptr;
  data.inbound_events.clear();
  data.remote_entities.clear();
  memset(&data.remote_enemy_buffer, 0, sizeof(data.remote_enemy_buffer));
  data.last_enemy_sync_time = 0;
  data.local_traffic_level_hash = 0;
  data.last_remote_traffic_level_hash = 0;
  multiplayer_reset_remote_traffic_buffers(data);
  multiplayer_reset_remote_palace_squid_state(data);
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
