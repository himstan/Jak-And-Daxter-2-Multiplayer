#include "multiplayer_pedestrian.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/sync/traffic_sync.h"
#include "common/log/log.h"
#include "enet/enet.h"
#include <cstring>

namespace {
size_t pedestrian_packet_size(uint32_t count) {
  return mp_traffic_packet_size(count, sizeof(MPPedestrianStatePacked));
}
}

bool handle_pedestrian_sync_packet(const _ENetEvent& event,
                                   MultiplayerData& data,
                                   uint32_t sender_player_id) {
  if (!event.packet) {
    return false;
  }
  uint32_t current_time = enet_time_get();
  if (!event.packet->data || event.packet->dataLength < pedestrian_packet_size(0)) {
    if (current_time - data.last_traffic_short_packet_debug_time > 2000) {
      lg::info("[Multiplayer] Short pedestrian traffic header. bytes={} need={}",
               event.packet->dataLength, pedestrian_packet_size(0));
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
  uint32_t ped_count = 0;
  uint32_t level_hash = 0;
  PacketHeader header = {};
  memcpy(&header, event.packet->data, sizeof(header));
  memcpy(&source_player_id, event.packet->data + source_offset, sizeof(source_player_id));
  memcpy(&authority_revision, event.packet->data + revision_offset, sizeof(authority_revision));
  memcpy(&ped_count, event.packet->data + count_offset, sizeof(ped_count));
  memcpy(&level_hash, event.packet->data + level_offset, sizeof(level_hash));
  if (header.type != PacketType::PEDESTRIAN_SYNC ||
      ped_count > MAX_PEDESTRIANS_PER_PACKET ||
      !mp_validate_traffic_source(data, source_player_id, sender_player_id, authority_revision) ||
      !mp_sequence_is_current_or_newer(
          header.sequenceNum,
          data.last_pedestrian_sequence_by_source[source_player_id])) {
    return false;
  }
  if (event.packet->dataLength != pedestrian_packet_size(ped_count)) {
    if (current_time - data.last_traffic_short_packet_debug_time > 2000) {
      lg::info("[Multiplayer] Short pedestrian traffic packet. bytes={} count={} need={}",
               event.packet->dataLength, ped_count, pedestrian_packet_size(ped_count));
      data.last_traffic_short_packet_debug_time = current_time;
    }
    return false;
  }
  for (uint32_t i = 0; i < ped_count; ++i) {
    MPPedestrianStatePacked incoming = {};
    memcpy(&incoming, event.packet->data + pedestrian_packet_size(0) + i * sizeof(incoming),
           sizeof(incoming));
    if (!mp_validate_pedestrian_net_id(incoming.net_id, source_player_id) ||
        (incoming.vehicle_net_id != 0 &&
         !mp_validate_vehicle_net_id(incoming.vehicle_net_id))) {
      return false;
    }
  }
  if (data.selected_traffic_authority != source_player_id) {
    data.last_pedestrian_sequence_by_source[source_player_id] = header.sequenceNum;
    return true;
  }
  if (!mp_accept_traffic_level(data, level_hash, ped_count, "pedestrian", current_time)) {
    data.last_pedestrian_sequence_by_source[source_player_id] = header.sequenceNum;
    return true;
  }
  data.last_pedestrian_sequence_by_source[source_player_id] = header.sequenceNum;
  if (current_time - data.last_ped_traffic_debug_time > 2000) {
    lg::info("[Multiplayer] Accepted pedestrian traffic. packetLevel={} remoteLevel={} count={}",
             level_hash, data.last_remote_traffic_level_hash, ped_count);
    data.last_ped_traffic_debug_time = current_time;
  }
  data.last_traffic_sync_time = current_time;
  for (uint32_t i = 0; i < ped_count; i++) {
    MPPedestrianStatePacked incoming = {};
    memcpy(&incoming, event.packet->data + pedestrian_packet_size(0) + i * sizeof(incoming),
           sizeof(incoming));
    if (incoming.net_id == 0) continue;
    if ((incoming.target_player_id != kMPInvalidCompactPlayerId &&
         incoming.target_player_id >= kMPMaxPlayers) ||
        !mp_float_is_finite(incoming.x) || !mp_float_is_finite(incoming.y) ||
        !mp_float_is_finite(incoming.z)) {
      continue;
    }
    auto* state = mp_find_matching_or_empty_slot(
        data.traffic_buffer.pedestrians,
        MAX_PEDESTRIAN_SYNC_COUNT,
        incoming.net_id,
        [](const MPPedestrianState& item) { return item.net_id; });
    if (state) {
      uint32_t slot = (uint32_t)(state - data.traffic_buffer.pedestrians);
      state->net_id = incoming.net_id;
      state->object_type = incoming.object_type;
      state->object_variance = incoming.object_variance;
      state->x = incoming.x; state->y = incoming.y; state->z = incoming.z;
      state->quat_x = mp_unpack_float_q(incoming.quat[0]);
      state->quat_y = mp_unpack_float_q(incoming.quat[1]);
      state->quat_z = mp_unpack_float_q(incoming.quat[2]);
      state->quat_w = mp_unpack_float_q(incoming.quat[3]);
      state->hp = incoming.hp;
      state->state_id = incoming.state_id;
      state->target_player_id = incoming.target_player_id;
      state->animation_profile = incoming.animation_profile;
      state->vehicle_net_id = incoming.vehicle_net_id;
      state->transport_id = incoming.transport_id;
      state->transport_side = incoming.transport_side;
      state->flags = incoming.flags;
      data.ped_last_updated[slot] = current_time;
    }
  }
  return true;
}

void send_pedestrian_sync_packets(MultiplayerData& data,
                                  MPTrafficSyncBufferGOAL* buffer,
                                  int channel) {
  const uint32_t total_peds =
      (buffer->ped_count < MAX_PEDESTRIAN_SYNC_COUNT)
          ? buffer->ped_count
          : MAX_PEDESTRIAN_SYNC_COUNT;

  if (total_peds == 0) {
    return;
  }

  const uint32_t snapshot_sequence = ++data.sequence_num;

  uint32_t sent_peds = 0;

  while (sent_peds < total_peds) {
    const uint32_t chunk_size =
        (total_peds - sent_peds < MAX_PEDESTRIANS_PER_PACKET)
            ? (total_peds - sent_peds)
            : MAX_PEDESTRIANS_PER_PACKET;

    PacketPedestrianSync packet = {};
    packet.header.type = PacketType::PEDESTRIAN_SYNC;
    packet.header.sequenceNum = snapshot_sequence;
    packet.source_player_id = static_cast<uint8_t>(data.local_player_id);
    packet.authority_revision = data.traffic_authority_revision;
    packet.count = chunk_size;
    packet.timestamp = enet_time_get();
    packet.level_hash = data.local_traffic_level_hash;

    for (uint32_t i = 0; i < chunk_size; i++) {
      auto* src = &buffer->pedestrians[sent_peds + i];
      auto* dst = &packet.peds[i];

      dst->net_id = src->net_id;
      dst->object_type = src->object_type;
      dst->object_variance = src->object_variance;
      dst->x = src->x;
      dst->y = src->y;
      dst->z = src->z;
      dst->quat[0] = mp_pack_float_q(src->quat_x);
      dst->quat[1] = mp_pack_float_q(src->quat_y);
      dst->quat[2] = mp_pack_float_q(src->quat_z);
      dst->quat[3] = mp_pack_float_q(src->quat_w);
      dst->hp = src->hp;
      dst->state_id = src->state_id;
      dst->target_player_id = src->target_player_id;
      dst->animation_profile = src->animation_profile;
      dst->vehicle_net_id = src->vehicle_net_id;
      dst->transport_id = src->transport_id;
      dst->transport_side = src->transport_side;
      dst->flags = src->flags;
      dst->pad[0] = 0;
      dst->pad[1] = 0;
    }

    const size_t packet_size = pedestrian_packet_size(chunk_size);

    MultiplayerManager::broadcast(
        data,
        channel,
        &packet,
        packet_size,
        ENET_PACKET_FLAG_UNSEQUENCED);

    sent_peds += chunk_size;
  }
}

void receive_pedestrian_sync_data(MultiplayerData& data,
                                  MPTrafficSyncBufferGOAL* buffer) {
  if (!buffer) {
    return;
  }

  uint32_t out_count = 0;

  for (uint32_t i = 0; i < MAX_PEDESTRIAN_SYNC_COUNT; ++i) {
    const auto& src = data.traffic_buffer.pedestrians[i];

    if (src.net_id == 0) {
      continue;
    }

    buffer->pedestrians[out_count++] = src;
  }

  for (uint32_t i = out_count; i < MAX_PEDESTRIAN_SYNC_COUNT; ++i) {
    buffer->pedestrians[i].net_id = 0;
  }

  buffer->ped_count = out_count;
}
