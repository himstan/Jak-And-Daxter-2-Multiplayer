#include "multiplayer_vehicle.h"
#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "common/log/log.h"
#include "enet/enet.h"
#include <cstring>

namespace {
inline int16_t pack_float_q(float v) {
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  return (int16_t)(v * 32767.0f);
}

inline float unpack_float_q(int16_t v) {
  return (float)v / 32767.0f;
}

size_t vehicle_packet_size(uint32_t count) {
  return sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t) +
         sizeof(uint32_t) +
         (sizeof(MPVehicleStatePacked) * count);
}
}

void handle_vehicle_sync_packet(const _ENetEvent& event, MultiplayerData& data) {
  if (!event.packet) {
    return;
  }
  uint32_t current_time = enet_time_get();
  PacketVehicleSync* sync = (PacketVehicleSync*)event.packet->data;
  uint32_t veh_count = (sync->count < (uint32_t)MAX_VEHICLES_PER_PACKET) ?
                       sync->count :
                       (uint32_t)MAX_VEHICLES_PER_PACKET;
  if (event.packet->dataLength < vehicle_packet_size(veh_count)) {
    if (current_time - data.last_traffic_short_packet_debug_time > 2000) {
      lg::info("[Multiplayer] Short vehicle traffic packet. bytes={} count={} need={}",
               event.packet->dataLength, veh_count, vehicle_packet_size(veh_count));
      data.last_traffic_short_packet_debug_time = current_time;
    }
    return;
  }
  if (sync->level_hash != 0 && data.last_remote_traffic_level_hash != 0 &&
      sync->level_hash != data.last_remote_traffic_level_hash) {
    if (current_time - data.last_traffic_drop_debug_time > 1000) {
      lg::info("[Multiplayer] Dropped vehicle traffic for level mismatch. packetLevel={} remoteLevel={} count={}",
               sync->level_hash, data.last_remote_traffic_level_hash, veh_count);
      data.last_traffic_drop_debug_time = current_time;
    }
    return;
  }
  if (sync->level_hash != 0 && data.remote_traffic_buffer_level_hash != 0 &&
      sync->level_hash != data.remote_traffic_buffer_level_hash) {
    memset(&data.traffic_buffer, 0, sizeof(data.traffic_buffer));
    memset(data.ped_last_updated, 0, sizeof(data.ped_last_updated));
    memset(data.veh_last_updated, 0, sizeof(data.veh_last_updated));
    lg::info("[Multiplayer] Reset remote traffic table for vehicle level change. old={} new={}",
             data.remote_traffic_buffer_level_hash, sync->level_hash);
  }
  if (sync->level_hash != 0) {
    data.remote_traffic_buffer_level_hash = sync->level_hash;
  }
  if (current_time - data.last_veh_traffic_debug_time > 2000) {
    lg::info("[Multiplayer] Accepted vehicle traffic. packetLevel={} remoteLevel={} count={}",
             sync->level_hash, data.last_remote_traffic_level_hash, veh_count);
    data.last_veh_traffic_debug_time = current_time;
  }
  data.last_traffic_sync_time = current_time;
  for (uint32_t i = 0; i < veh_count; i++) {
    auto* incoming = &sync->vehs[i];
    if (incoming->net_id == 0) continue;
    bool found = false;
    for (uint32_t j = 0; j < MAX_VEHICLE_SYNC_COUNT; j++) {
      if (data.traffic_buffer.vehicles[j].net_id == incoming->net_id) {
        auto& state = data.traffic_buffer.vehicles[j];
        state.net_id = incoming->net_id;
        state.vehicle_type = incoming->vehicle_type;
        state.color_index = incoming->color_index;
        state.state_id = incoming->state_id;
        state.target_aid = incoming->target_aid;
        state.x = incoming->x; state.y = incoming->y; state.z = incoming->z;
        state.quat_x = unpack_float_q(incoming->quat[0]);
        state.quat_y = unpack_float_q(incoming->quat[1]);
        state.quat_z = unpack_float_q(incoming->quat[2]);
        state.quat_w = unpack_float_q(incoming->quat[3]);
        state.lin_vel_x = (float)incoming->lin_vel[0] / 10.0f;
        state.lin_vel_y = (float)incoming->lin_vel[1] / 10.0f;
        state.lin_vel_z = (float)incoming->lin_vel[2] / 10.0f;
        state.ang_vel_x = unpack_float_q(incoming->ang_vel[0]) * 10.0f;
        state.ang_vel_y = unpack_float_q(incoming->ang_vel[1]) * 10.0f;
        state.ang_vel_z = unpack_float_q(incoming->ang_vel[2]) * 10.0f;
        state.state_flags = incoming->state_flags;
        memcpy(state.rider_aids, incoming->rider_aids, 16);
        data.veh_last_updated[j] = current_time;
        found = true; break;
      }
    }
    if (!found) {
      for (uint32_t j = 0; j < MAX_VEHICLE_SYNC_COUNT; j++) {
        if (data.traffic_buffer.vehicles[j].net_id == 0) {
          auto& state = data.traffic_buffer.vehicles[j];
          state.net_id = incoming->net_id;
          state.vehicle_type = incoming->vehicle_type;
          state.color_index = incoming->color_index;
          state.state_id = incoming->state_id;
          state.target_aid = incoming->target_aid;
          state.x = incoming->x; state.y = incoming->y; state.z = incoming->z;
          state.quat_x = unpack_float_q(incoming->quat[0]);
          state.quat_y = unpack_float_q(incoming->quat[1]);
          state.quat_z = unpack_float_q(incoming->quat[2]);
          state.quat_w = unpack_float_q(incoming->quat[3]);
          state.lin_vel_x = (float)incoming->lin_vel[0] / 10.0f;
          state.lin_vel_y = (float)incoming->lin_vel[1] / 10.0f;
          state.lin_vel_z = (float)incoming->lin_vel[2] / 10.0f;
          state.ang_vel_x = unpack_float_q(incoming->ang_vel[0]) * 10.0f;
          state.ang_vel_y = unpack_float_q(incoming->ang_vel[1]) * 10.0f;
          state.ang_vel_z = unpack_float_q(incoming->ang_vel[2]) * 10.0f;
          state.state_flags = incoming->state_flags;
          memcpy(state.rider_aids, incoming->rider_aids, 16);
          data.veh_last_updated[j] = current_time;
          found = true; break;
        }
      }
    }
  }
}

void send_vehicle_sync_packets(MultiplayerData& data, MPTrafficSyncBufferGOAL* buffer, int exclude_peer) {
  uint32_t total_vehs = (buffer->veh_count < MAX_VEHICLE_SYNC_COUNT) ? buffer->veh_count : MAX_VEHICLE_SYNC_COUNT;
  uint32_t sent_vehs = 0;
  while (sent_vehs < total_vehs) {
    uint32_t chunk_size = (total_vehs - sent_vehs < MAX_VEHICLES_PER_PACKET) ? (total_vehs - sent_vehs) : MAX_VEHICLES_PER_PACKET;
    PacketVehicleSync packet; packet.header.type = PacketType::VEHICLE_SYNC;
    packet.header.sequenceNum = ++data.sequence_num;
    packet.count = chunk_size; packet.timestamp = enet_time_get();
    packet.level_hash = data.local_traffic_level_hash;
    for (uint32_t i = 0; i < chunk_size; i++) {
      auto* src = &buffer->vehicles[sent_vehs + i]; auto* dst = &packet.vehs[i];
      dst->net_id = src->net_id; dst->vehicle_type = src->vehicle_type; dst->color_index = src->color_index;
      dst->state_id = src->state_id; dst->target_aid = src->target_aid;
      dst->x = src->x; dst->y = src->y; dst->z = src->z;
      dst->quat[0] = pack_float_q(src->quat_x); dst->quat[1] = pack_float_q(src->quat_y);
      dst->quat[2] = pack_float_q(src->quat_z); dst->quat[3] = pack_float_q(src->quat_w);
      dst->lin_vel[0] = (int16_t)(src->lin_vel_x * 10.0f); dst->lin_vel[1] = (int16_t)(src->lin_vel_y * 10.0f); dst->lin_vel[2] = (int16_t)(src->lin_vel_z * 10.0f);
      dst->ang_vel[0] = pack_float_q(src->ang_vel_x / 10.0f); dst->ang_vel[1] = pack_float_q(src->ang_vel_y / 10.0f); dst->ang_vel[2] = pack_float_q(src->ang_vel_z / 10.0f);
      dst->state_flags = src->state_flags; memcpy(dst->rider_aids, src->rider_aids, 16);
    }
    size_t packet_size = vehicle_packet_size(chunk_size);
    MultiplayerManager::broadcast(data, exclude_peer, &packet, packet_size, ENET_PACKET_FLAG_UNSEQUENCED);
    sent_vehs += chunk_size;
  }
}

void receive_vehicle_sync_data(MultiplayerData& data, MPTrafficSyncBufferGOAL* buffer) {
  uint32_t active_count = 0;
  for (uint32_t i = 0; i < MAX_VEHICLE_SYNC_COUNT; i++) {
    if (data.traffic_buffer.vehicles[i].net_id != 0) {
      active_count++;
    }
  }
  buffer->veh_count = active_count;
  memcpy(buffer->vehicles, data.traffic_buffer.vehicles, sizeof(MPVehicleState) * MAX_VEHICLE_SYNC_COUNT);
}
