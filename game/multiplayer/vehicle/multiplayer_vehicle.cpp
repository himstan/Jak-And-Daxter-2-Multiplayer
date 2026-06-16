#include "multiplayer_vehicle.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/sync/traffic_sync.h"
#include "common/log/log.h"
#include "enet/enet.h"
#include <cstring>

namespace {
size_t vehicle_packet_size(uint32_t count) {
  return mp_traffic_packet_size(count, sizeof(MPVehicleStatePacked));
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
  if (!mp_accept_traffic_level(data, sync->level_hash, veh_count, "vehicle", current_time)) {
    return;
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
    auto* state = mp_find_matching_or_empty_slot(
        data.traffic_buffer.vehicles,
        MAX_VEHICLE_SYNC_COUNT,
        incoming->net_id,
        [](const MPVehicleState& item) { return item.net_id; });
    if (state) {
      uint32_t slot = (uint32_t)(state - data.traffic_buffer.vehicles);
      state->net_id = incoming->net_id;
      state->vehicle_type = incoming->vehicle_type;
      state->color_index = incoming->color_index;
      state->state_id = incoming->state_id;
      state->target_aid = incoming->target_aid;
      state->x = incoming->x; state->y = incoming->y; state->z = incoming->z;
      state->quat_x = mp_unpack_float_q(incoming->quat[0]);
      state->quat_y = mp_unpack_float_q(incoming->quat[1]);
      state->quat_z = mp_unpack_float_q(incoming->quat[2]);
      state->quat_w = mp_unpack_float_q(incoming->quat[3]);
      state->lin_vel_x = (float)incoming->lin_vel[0] / 10.0f;
      state->lin_vel_y = (float)incoming->lin_vel[1] / 10.0f;
      state->lin_vel_z = (float)incoming->lin_vel[2] / 10.0f;
      state->ang_vel_x = mp_unpack_float_q(incoming->ang_vel[0]) * 10.0f;
      state->ang_vel_y = mp_unpack_float_q(incoming->ang_vel[1]) * 10.0f;
      state->ang_vel_z = mp_unpack_float_q(incoming->ang_vel[2]) * 10.0f;
      state->state_flags = incoming->state_flags;
      memcpy(state->rider_aids, incoming->rider_aids, sizeof(state->rider_aids));
      data.veh_last_updated[slot] = current_time;
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
      dst->quat[0] = mp_pack_float_q(src->quat_x); dst->quat[1] = mp_pack_float_q(src->quat_y);
      dst->quat[2] = mp_pack_float_q(src->quat_z); dst->quat[3] = mp_pack_float_q(src->quat_w);
      dst->lin_vel[0] = (int16_t)(src->lin_vel_x * 10.0f); dst->lin_vel[1] = (int16_t)(src->lin_vel_y * 10.0f); dst->lin_vel[2] = (int16_t)(src->lin_vel_z * 10.0f);
      dst->ang_vel[0] = mp_pack_float_q(src->ang_vel_x / 10.0f); dst->ang_vel[1] = mp_pack_float_q(src->ang_vel_y / 10.0f); dst->ang_vel[2] = mp_pack_float_q(src->ang_vel_z / 10.0f);
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
