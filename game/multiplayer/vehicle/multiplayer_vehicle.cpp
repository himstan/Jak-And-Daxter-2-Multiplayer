#include "multiplayer_vehicle.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/sync/traffic_sync.h"
#include "common/log/log.h"
#include "enet/enet.h"
#include <cstddef>
#include <cstring>

namespace {
bool valid_compact_player_id(uint8_t player_id) {
  return player_id < kMPMaxPlayers || player_id == kMPInvalidCompactPlayerId;
}

bool valid_vehicle_rider_id(uint32_t player_id) {
  return player_id < kMPMaxPlayers || player_id == kMPInvalidPlayerId ||
         player_id == kMPVehicleCivilianRiderId;
}

bool valid_vehicle_occupants(const MPVehicleStatePacked& state) {
  if (!valid_compact_player_id(state.target_player_id)) {
    return false;
  }
  for (const auto player_id : state.rider_player_ids) {
    if (!valid_vehicle_rider_id(player_id)) {
      return false;
    }
  }
  return true;
}

size_t vehicle_packet_size(uint32_t count) {
  return offsetof(PacketVehicleSync, vehs) + (sizeof(MPVehicleStatePacked) * count);
}

}

bool handle_vehicle_sync_packet(const _ENetEvent& event,
                                MultiplayerData& data,
                                uint32_t sender_player_id) {
  if (!event.packet) {
    return false;
  }
  uint32_t current_time = enet_time_get();
  if (!event.packet->data || event.packet->dataLength < vehicle_packet_size(0)) {
    if (current_time - data.last_traffic_short_packet_debug_time > 2000) {
      lg::info("[Multiplayer] Short vehicle traffic header. bytes={} need={}",
               event.packet->dataLength, vehicle_packet_size(0));
      data.last_traffic_short_packet_debug_time = current_time;
    }
    return false;
  }
  constexpr size_t source_offset = sizeof(PacketHeader);
  constexpr size_t revision_offset = source_offset + sizeof(uint8_t) + 3;
  constexpr size_t count_offset = revision_offset + sizeof(uint32_t);
  constexpr size_t level_offset = count_offset + sizeof(uint32_t) + sizeof(uint64_t);
  uint8_t source_player_id = kMPInvalidCompactPlayerId;
  uint32_t authority_revision = 0;
  uint32_t veh_count = 0;
  uint32_t level_hash = 0;
  PacketHeader header = {};
  memcpy(&header, event.packet->data, sizeof(header));
  memcpy(&source_player_id, event.packet->data + source_offset, sizeof(source_player_id));
  memcpy(&authority_revision, event.packet->data + revision_offset, sizeof(authority_revision));
  memcpy(&veh_count, event.packet->data + count_offset, sizeof(veh_count));
  memcpy(&level_hash, event.packet->data + level_offset, sizeof(level_hash));
  if (header.type != PacketType::VEHICLE_SYNC || veh_count > MAX_VEHICLES_PER_PACKET ||
      !mp_validate_traffic_source(data, source_player_id, sender_player_id, authority_revision) ||
      !mp_sequence_is_current_or_newer(
          header.sequenceNum,
          data.last_vehicle_sequence_by_source[source_player_id])) {
    return false;
  }
  if (event.packet->dataLength != vehicle_packet_size(veh_count)) {
    if (current_time - data.last_traffic_short_packet_debug_time > 2000) {
      lg::info("[Multiplayer] Short vehicle traffic packet. bytes={} count={} need={}",
               event.packet->dataLength, veh_count, vehicle_packet_size(veh_count));
      data.last_traffic_short_packet_debug_time = current_time;
    }
    return false;
  }
  for (uint32_t i = 0; i < veh_count; ++i) {
    MPVehicleStatePacked incoming = {};
    memcpy(&incoming, event.packet->data + vehicle_packet_size(0) + i * sizeof(incoming),
           sizeof(incoming));
    if (!mp_validate_vehicle_net_id(incoming.net_id)) {
      return false;
    }
  }
  if (data.selected_traffic_authority != source_player_id) {
    data.last_vehicle_sequence_by_source[source_player_id] = header.sequenceNum;
    return true;
  }
  if (!mp_accept_traffic_level(data, level_hash, veh_count, "vehicle", current_time)) {
    data.last_vehicle_sequence_by_source[source_player_id] = header.sequenceNum;
    return true;
  }
  data.last_vehicle_sequence_by_source[source_player_id] = header.sequenceNum;
  if (current_time - data.last_veh_traffic_debug_time > 2000) {
    lg::info("[Multiplayer] Accepted vehicle traffic. packetLevel={} remoteLevel={} count={}",
             level_hash, data.last_remote_traffic_level_hash, veh_count);
    data.last_veh_traffic_debug_time = current_time;
  }
  data.last_traffic_sync_time = current_time;
  for (uint32_t i = 0; i < veh_count; i++) {
    MPVehicleStatePacked incoming = {};
    memcpy(&incoming, event.packet->data + vehicle_packet_size(0) + i * sizeof(incoming),
           sizeof(incoming));
    if (incoming.net_id == 0 || !valid_vehicle_occupants(incoming)) continue;
    if (!mp_float_is_finite(incoming.x) || !mp_float_is_finite(incoming.y) ||
        !mp_float_is_finite(incoming.z)) {
      continue;
    }
    auto* state = mp_find_matching_or_empty_slot(
        data.traffic_buffer.vehicles,
        MAX_VEHICLE_SYNC_COUNT,
        incoming.net_id,
        [](const MPVehicleState& item) { return item.net_id; });
    if (state) {
      uint32_t slot = (uint32_t)(state - data.traffic_buffer.vehicles);
      if (state->net_id != incoming.net_id) {
        data.veh_last_updated[slot] = 0;
        data.veh_last_sequence[slot] = 0;
      }
      if (!mp_sequence_is_newer(header.sequenceNum, data.veh_last_sequence[slot])) {
        continue;
      }
      state->net_id = incoming.net_id;
      state->vehicle_type = incoming.vehicle_type;
      state->color_index = incoming.color_index;
      state->state_id = incoming.state_id;
      state->target_player_id = incoming.target_player_id;
      state->x = incoming.x; state->y = incoming.y; state->z = incoming.z;
      state->quat_x = mp_unpack_float_q(incoming.quat[0]);
      state->quat_y = mp_unpack_float_q(incoming.quat[1]);
      state->quat_z = mp_unpack_float_q(incoming.quat[2]);
      state->quat_w = mp_unpack_float_q(incoming.quat[3]);
      state->lin_vel_x = (float)incoming.lin_vel[0] / 10.0f;
      state->lin_vel_y = (float)incoming.lin_vel[1] / 10.0f;
      state->lin_vel_z = (float)incoming.lin_vel[2] / 10.0f;
      state->ang_vel_x = mp_unpack_float_q(incoming.ang_vel[0]) * 10.0f;
      state->ang_vel_y = mp_unpack_float_q(incoming.ang_vel[1]) * 10.0f;
      state->ang_vel_z = mp_unpack_float_q(incoming.ang_vel[2]) * 10.0f;
      state->state_flags = incoming.state_flags;
      state->hit_points = incoming.hit_points;
      memcpy(state->rider_player_ids, incoming.rider_player_ids,
             sizeof(state->rider_player_ids));
      data.veh_last_updated[slot] = current_time;
      data.veh_last_sequence[slot] = header.sequenceNum;
    }
  }
  return true;
}

void send_vehicle_sync_packets(MultiplayerData& data,
                               MPTrafficSyncBufferGOAL* buffer,
                               int channel) {
  const uint32_t total_vehs =
        (buffer->veh_count < MAX_VEHICLE_SYNC_COUNT)
            ? buffer->veh_count
            : MAX_VEHICLE_SYNC_COUNT;
  if (total_vehs == 0) {
    return;
  }

  const uint32_t snapshot_sequence = ++data.sequence_num;

  uint32_t sent_vehs = 0;

  while (sent_vehs < total_vehs) {
    const uint32_t chunk_size =
        (total_vehs - sent_vehs < MAX_VEHICLES_PER_PACKET)
            ? (total_vehs - sent_vehs)
            : MAX_VEHICLES_PER_PACKET;

    PacketVehicleSync packet = {};
    packet.header.type = PacketType::VEHICLE_SYNC;
    packet.header.sequenceNum = snapshot_sequence;
    packet.source_player_id = static_cast<uint8_t>(data.local_player_id);
    packet.authority_revision = data.traffic_authority_revision;
    packet.count = chunk_size;
    packet.timestamp = enet_time_get();
    packet.level_hash = data.local_traffic_level_hash;
    for (uint32_t i = 0; i < chunk_size; i++) {
      auto* src = &buffer->vehicles[sent_vehs + i]; auto* dst = &packet.vehs[i];
      dst->net_id = src->net_id; dst->vehicle_type = src->vehicle_type; dst->color_index = src->color_index;
      dst->state_id = src->state_id; dst->target_player_id = src->target_player_id;
      dst->x = src->x; dst->y = src->y; dst->z = src->z;
      dst->quat[0] = mp_pack_float_q(src->quat_x); dst->quat[1] = mp_pack_float_q(src->quat_y);
      dst->quat[2] = mp_pack_float_q(src->quat_z); dst->quat[3] = mp_pack_float_q(src->quat_w);
      dst->lin_vel[0] = mp_pack_float_scaled(src->lin_vel_x, 10.0f);
      dst->lin_vel[1] = mp_pack_float_scaled(src->lin_vel_y, 10.0f);
      dst->lin_vel[2] = mp_pack_float_scaled(src->lin_vel_z, 10.0f);
      dst->ang_vel[0] = mp_pack_float_q(src->ang_vel_x / 10.0f); dst->ang_vel[1] = mp_pack_float_q(src->ang_vel_y / 10.0f); dst->ang_vel[2] = mp_pack_float_q(src->ang_vel_z / 10.0f);
      dst->state_flags = src->state_flags;
      dst->hit_points = src->hit_points;
      memcpy(dst->rider_player_ids, src->rider_player_ids, 16);
    }
    const size_t packet_size = vehicle_packet_size(chunk_size);
    
    MultiplayerManager::broadcast(
        data,
        channel,
        &packet,
        packet_size,
        ENET_PACKET_FLAG_UNSEQUENCED);

    sent_vehs += chunk_size;
  }
}

void receive_vehicle_sync_data(MultiplayerData& data,
                               MPTrafficSyncBufferGOAL* buffer) {
  if (!buffer) {
    return;
  }

  uint32_t out_count = 0;

  for (uint32_t i = 0; i < MAX_VEHICLE_SYNC_COUNT; ++i) {
    const auto& src = data.traffic_buffer.vehicles[i];

    if (src.net_id == 0) {
      continue;
    }

    buffer->vehicles[out_count] = src;
    ++out_count;
  }

  for (uint32_t i = out_count; i < MAX_VEHICLE_SYNC_COUNT; ++i) {
    buffer->vehicles[i].net_id = 0;
  }

  buffer->veh_count = out_count;
}
