#include "multiplayer_pedestrian.h"
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

inline uint32_t pedestrian_vehicle_net_id(const MPPedestrianState* state) {
  return ((uint32_t)state->pad[0]) |
         ((uint32_t)state->pad[1] << 8) |
         ((uint32_t)state->pad[2] << 16) |
         ((uint32_t)state->pad[3] << 24);
}

inline void set_pedestrian_vehicle_net_id(MPPedestrianState& state, uint32_t vehicle_net_id) {
  state.pad[0] = (uint8_t)(vehicle_net_id & 0xff);
  state.pad[1] = (uint8_t)((vehicle_net_id >> 8) & 0xff);
  state.pad[2] = (uint8_t)((vehicle_net_id >> 16) & 0xff);
  state.pad[3] = (uint8_t)((vehicle_net_id >> 24) & 0xff);
}

size_t pedestrian_packet_size(uint32_t count) {
  return sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t) +
         (sizeof(MPPedestrianStatePacked) * count);
}
}

void handle_pedestrian_sync_packet(const _ENetEvent& event, MultiplayerData& data) {
  if (!event.packet) {
    return;
  }
  uint32_t current_time = enet_time_get();
  PacketPedestrianSync* sync = (PacketPedestrianSync*)event.packet->data;
  uint32_t ped_count = (sync->count < (uint32_t)MAX_PEDESTRIANS_PER_PACKET) ?
                       sync->count :
                       (uint32_t)MAX_PEDESTRIANS_PER_PACKET;
  if (event.packet->dataLength < pedestrian_packet_size(ped_count)) {
    return;
  }
  data.last_traffic_sync_time = current_time;
  for (uint32_t i = 0; i < ped_count; i++) {
    auto* incoming = &sync->peds[i];
    if (incoming->net_id == 0) continue;
    bool found = false;
    for (uint32_t j = 0; j < MAX_PEDESTRIAN_SYNC_COUNT; j++) {
      if (data.traffic_buffer.pedestrians[j].net_id == incoming->net_id) {
        auto& state = data.traffic_buffer.pedestrians[j];
        state.net_id = incoming->net_id;
        state.object_type = incoming->object_type;
        state.object_variance = incoming->object_variance;
        state.x = incoming->x; state.y = incoming->y; state.z = incoming->z;
        state.quat_x = unpack_float_q(incoming->quat[0]);
        state.quat_y = unpack_float_q(incoming->quat[1]);
        state.quat_z = unpack_float_q(incoming->quat[2]);
        state.quat_w = unpack_float_q(incoming->quat[3]);
        state.hp = incoming->hp;
        state.state_id = incoming->state_id;
        state.target_aid = incoming->target_aid;
        set_pedestrian_vehicle_net_id(state, incoming->vehicle_net_id);
        data.ped_last_updated[j] = current_time;
        found = true; break;
      }
    }
    if (!found) {
      for (uint32_t j = 0; j < MAX_PEDESTRIAN_SYNC_COUNT; j++) {
        if (data.traffic_buffer.pedestrians[j].net_id == 0) {
          auto& state = data.traffic_buffer.pedestrians[j];
          state.net_id = incoming->net_id;
          state.object_type = incoming->object_type;
          state.object_variance = incoming->object_variance;
          state.x = incoming->x; state.y = incoming->y; state.z = incoming->z;
          state.quat_x = unpack_float_q(incoming->quat[0]);
          state.quat_y = unpack_float_q(incoming->quat[1]);
          state.quat_z = unpack_float_q(incoming->quat[2]);
          state.quat_w = unpack_float_q(incoming->quat[3]);
          state.hp = incoming->hp;
          state.state_id = incoming->state_id;
          state.target_aid = incoming->target_aid;
          set_pedestrian_vehicle_net_id(state, incoming->vehicle_net_id);
          data.ped_last_updated[j] = current_time;
          found = true; break;
        }
      }
    }
  }
}

void send_pedestrian_sync_packets(MultiplayerData& data, MPTrafficSyncBufferGOAL* buffer, int exclude_peer) {
  uint32_t total_peds = (buffer->ped_count < MAX_PEDESTRIAN_SYNC_COUNT) ? buffer->ped_count : MAX_PEDESTRIAN_SYNC_COUNT;
  uint32_t sent_peds = 0;
  while (sent_peds < total_peds) {
    uint32_t chunk_size = (total_peds - sent_peds < MAX_PEDESTRIANS_PER_PACKET) ? (total_peds - sent_peds) : MAX_PEDESTRIANS_PER_PACKET;
    PacketPedestrianSync packet; packet.header.type = PacketType::PEDESTRIAN_SYNC;
    packet.header.sequenceNum = ++data.sequence_num;
    packet.count = chunk_size; packet.timestamp = enet_time_get();
    for (uint32_t i = 0; i < chunk_size; i++) {
      auto* src = &buffer->pedestrians[sent_peds + i]; auto* dst = &packet.peds[i];
      dst->net_id = src->net_id; dst->object_type = src->object_type; dst->object_variance = src->object_variance;
      dst->x = src->x; dst->y = src->y; dst->z = src->z;
      dst->quat[0] = pack_float_q(src->quat_x); dst->quat[1] = pack_float_q(src->quat_y);
      dst->quat[2] = pack_float_q(src->quat_z); dst->quat[3] = pack_float_q(src->quat_w);
      dst->hp = src->hp;
      dst->state_id = src->state_id;
      dst->target_aid = src->target_aid;
      dst->vehicle_net_id = pedestrian_vehicle_net_id(src);
      dst->pad[0] = 0;
      dst->pad[1] = 0;
    }
    size_t packet_size = sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t) + (sizeof(MPPedestrianStatePacked) * chunk_size);
    MultiplayerManager::broadcast(data, exclude_peer, &packet, packet_size, ENET_PACKET_FLAG_UNSEQUENCED);
    sent_peds += chunk_size;
  }
}

void receive_pedestrian_sync_data(MultiplayerData& data, MPTrafficSyncBufferGOAL* buffer) {
  uint32_t active_count = 0;
  for (uint32_t i = 0; i < MAX_PEDESTRIAN_SYNC_COUNT; i++) {
    if (data.traffic_buffer.pedestrians[i].net_id != 0) {
      active_count++;
    }
  }
  buffer->ped_count = active_count;
  memcpy(buffer->pedestrians, data.traffic_buffer.pedestrians, sizeof(MPPedestrianState) * MAX_PEDESTRIAN_SYNC_COUNT);
}
