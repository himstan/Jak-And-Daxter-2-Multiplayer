#pragma once

#include <array>
#include <vector>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/jak2/wire/wire_policy.h"
#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/quantization.h"

namespace multiplayer::jak2::wire {

struct PedestrianStateRecord {
  uint32_t net_id = 0;
  uint8_t object_type = 0;
  uint8_t object_variance = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {};
  int32_t hit_points = 0;
  uint32_t state_id = 0;
  uint8_t flags = 0;
  uint8_t target_player_id = kWireInvalidPlayerId;
  uint16_t animation_profile = 0;
  uint32_t vehicle_net_id = 0;
  uint32_t transport_id = 0;
  uint8_t transport_side = 0;
  uint16_t level_id = 0;
};

struct PedestrianStateBatchPacket
    : platform::wire::Packet<PedestrianStateBatchPacket, PacketType::PEDESTRIAN_STATE_BATCH> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "PEDESTRIAN_STATE_BATCH",
      .priority = platform::MessagePriority::BULK,
      .cadence = platform::CadenceMode::PERIODIC,
      .interval_ms = 66,
      .maximum_payload_bytes = 4493};

  uint32_t sample_time_ms = 0;
  uint32_t authority_revision = 0;
  uint32_t level_id = 0;
  std::vector<PedestrianStateRecord> pedestrians;
};

template <typename Stream, typename RecordT>
bool serialize_pedestrian_state_record(Stream& stream, RecordT&& pedestrian) {
  if constexpr (Stream::IsReading) {
    pedestrian = {};
  }
  return platform::wire::serialize_u32(stream, pedestrian.net_id) &&
         platform::wire::serialize_u8(stream, pedestrian.object_type) &&
         platform::wire::serialize_u8(stream, pedestrian.object_variance) &&
         platform::wire::serialize_position_array(stream, pedestrian.position) &&
         platform::wire::serialize_quaternion_array(stream, pedestrian.quaternion) &&
         platform::wire::serialize_uint_bits(stream, pedestrian.hit_points, 8) &&
         platform::wire::serialize_uint_bits(stream, pedestrian.state_id, 6) &&
         platform::wire::serialize_uint_bits(stream, pedestrian.flags, 2) &&
         platform::wire::serialize_uint_bits(stream, pedestrian.target_player_id, 4) &&
         platform::wire::serialize_u16(stream, pedestrian.animation_profile) &&
         platform::wire::serialize_u32(stream, pedestrian.vehicle_net_id) &&
         platform::wire::serialize_u32(stream, pedestrian.transport_id) &&
         platform::wire::serialize_u8(stream, pedestrian.transport_side) &&
         platform::wire::serialize_u16(stream, pedestrian.level_id) &&
         valid_wire_player_reference(pedestrian.target_player_id);
}

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, PedestrianStateBatchPacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  } else if (packet.pedestrians.size() > core::kMaxPedestrians) {
    return false;
  }

  if (!platform::wire::serialize_u32(stream, packet.sample_time_ms)) {
    return false;
  }
  uint8_t count = static_cast<uint8_t>(packet.pedestrians.size());
  if (!platform::wire::serialize_u8(stream, count) ||
      !platform::wire::serialize_u32(stream, packet.authority_revision) ||
      !platform::wire::serialize_u32(stream, packet.level_id)) {
    return false;
  }
  if constexpr (Stream::IsReading) {
    if (count > core::kMaxPedestrians) {
      return false;
    }
    packet.pedestrians.reserve(count);
    for (uint8_t index = 0; index < count; ++index) {
      PedestrianStateRecord pedestrian;
      if (!serialize_pedestrian_state_record(stream, pedestrian)) {
        return false;
      }
      packet.pedestrians.push_back(pedestrian);
    }
  } else {
    for (auto& pedestrian : packet.pedestrians) {
      if (!serialize_pedestrian_state_record(stream, pedestrian)) {
        return false;
      }
    }
  }
  return platform::wire::serialize_byte_align(stream);
}

bool validate_packet(const PedestrianStateBatchPacket& packet);

PedestrianStateBatchPacket to_pedestrian_state_batch_packet(const core::TrafficSnapshot& snapshot);
void from_packet(const PedestrianStateBatchPacket& packet, core::TrafficSnapshot& snapshot);

}  // namespace multiplayer::jak2::wire
