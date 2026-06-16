#include "traffic_sync.h"

#include "common/log/log.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_session.h"
#include "game/multiplayer/pedestrian/multiplayer_pedestrian.h"
#include "game/multiplayer/vehicle/multiplayer_vehicle.h"

size_t mp_traffic_packet_size(uint32_t count, size_t element_size) {
  return sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t) +
         (element_size * count);
}

bool mp_accept_traffic_level(MultiplayerData& data,
                             uint32_t level_hash,
                             uint32_t count,
                             const char* label,
                             uint32_t current_time) {
  if (level_hash != 0 && data.last_remote_traffic_level_hash != 0 &&
      level_hash != data.last_remote_traffic_level_hash) {
    if (current_time - data.last_traffic_drop_debug_time > 1000) {
      lg::info("[Multiplayer] Dropped {} traffic for level mismatch. packetLevel={} remoteLevel={} count={}",
               label, level_hash, data.last_remote_traffic_level_hash, count);
      data.last_traffic_drop_debug_time = current_time;
    }
    return false;
  }

  if (level_hash != 0 && data.remote_traffic_buffer_level_hash != 0 &&
      level_hash != data.remote_traffic_buffer_level_hash) {
    uint32_t old_level = data.remote_traffic_buffer_level_hash;
    multiplayer_reset_remote_traffic_buffers(data);
    lg::info("[Multiplayer] Reset remote traffic table for {} level change. old={} new={}",
             label, old_level, level_hash);
  }
  if (level_hash != 0) {
    data.remote_traffic_buffer_level_hash = level_hash;
  }
  return true;
}

void mp_send_traffic_sync(MultiplayerData& data, MPTrafficSyncBufferGOAL* buffer) {
  if (!buffer || data.local_role != 0) {
    return;
  }
  int exclude_peer = data.local_role;
  send_pedestrian_sync_packets(data, buffer, exclude_peer);
  send_vehicle_sync_packets(data, buffer, exclude_peer);
}

void mp_receive_traffic_sync(MultiplayerData& data, MPTrafficSyncBufferGOAL* buffer) {
  if (!buffer) {
    return;
  }
  receive_pedestrian_sync_data(data, buffer);
  receive_vehicle_sync_data(data, buffer);
}
