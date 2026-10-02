#pragma once

#include <array>
#include <vector>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/jak2/wire/wire_policy.h"
#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/quantization.h"

namespace multiplayer::jak2::wire {

inline constexpr size_t kVehicleStateBatchPacketPrefixWireSize = 13;
inline constexpr size_t kVehicleStateRecordWireBits = 381;
inline constexpr size_t kVehicleStateBatchPacketMaxWireSize = 3061;
inline constexpr size_t kVehicleStateRecordSeatCount = 3;

struct VehicleStateRecord {
  uint32_t net_id = 0;
  uint8_t vehicle_type = 0;
  uint8_t color_index = 0;
  uint8_t state_id = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {};
  std::array<float, 3> linear_velocity = {};
  std::array<float, 3> angular_velocity = {};
  uint8_t state_flags = 0;
  uint8_t hit_points = 0;
  uint16_t level_id = 0;
  std::array<uint8_t, kVehicleStateRecordSeatCount> rider_player_ids = {};
};

struct VehicleStateBatchPacket
    : platform::wire::Packet<VehicleStateBatchPacket, PacketType::VEHICLE_STATE_BATCH> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "VEHICLE_STATE_BATCH",
      .priority = platform::MessagePriority::BULK,
      .cadence = platform::CadenceMode::PERIODIC,
      .interval_ms = 66,
      .maximum_payload_bytes = kVehicleStateBatchPacketMaxWireSize};

  uint32_t sample_time_ms = 0;
  uint32_t authority_revision = 0;
  uint32_t level_id = 0;
  std::vector<VehicleStateRecord> vehicles;
};

template <typename Stream, typename RecordT>
bool serialize_vehicle_state_record(Stream& stream, RecordT&& vehicle) {
  if constexpr (Stream::IsReading) {
    vehicle = {};
  }
  if (!platform::wire::serialize_u32(stream, vehicle.net_id) ||
      !platform::wire::serialize_u8(stream, vehicle.vehicle_type) ||
      !platform::wire::serialize_u8(stream, vehicle.color_index) ||
      !platform::wire::serialize_uint_bits(stream, vehicle.state_id, 5) ||
      !platform::wire::serialize_vehicle_position_array(stream, vehicle.position) ||
      !platform::wire::serialize_vehicle_quaternion_array(stream, vehicle.quaternion) ||
      !platform::wire::serialize_linear_velocity_array(stream, vehicle.linear_velocity) ||
      !platform::wire::serialize_angular_velocity_array(stream, vehicle.angular_velocity) ||
      !platform::wire::serialize_uint_bits(stream, vehicle.state_flags, 5) ||
      !platform::wire::serialize_u8(stream, vehicle.hit_points) ||
      !platform::wire::serialize_u16(stream, vehicle.level_id)) {
    return false;
  }
  for (auto& rider : vehicle.rider_player_ids) {
    if (!platform::wire::serialize_uint_bits(stream, rider, 4) ||
        !valid_wire_rider_reference(rider)) {
      return false;
    }
  }
  return true;
}

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, VehicleStateBatchPacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  } else if (packet.vehicles.size() > core::kMaxVehicles) {
    return false;
  }

  if (!platform::wire::serialize_u32(stream, packet.sample_time_ms)) {
    return false;
  }
  uint8_t count = static_cast<uint8_t>(packet.vehicles.size());
  if (!platform::wire::serialize_uint_bits(stream, count, 7) ||
      !platform::wire::serialize_byte_align(stream) ||
      !platform::wire::serialize_u32(stream, packet.authority_revision) ||
      !platform::wire::serialize_u32(stream, packet.level_id)) {
    return false;
  }
  if constexpr (Stream::IsReading) {
    if (count > core::kMaxVehicles) {
      return false;
    }
    packet.vehicles.reserve(count);
    for (uint8_t index = 0; index < count; ++index) {
      VehicleStateRecord vehicle;
      if (!serialize_vehicle_state_record(stream, vehicle)) {
        return false;
      }
      packet.vehicles.push_back(vehicle);
    }
  } else {
    for (auto& vehicle : packet.vehicles) {
      if (!serialize_vehicle_state_record(stream, vehicle)) {
        return false;
      }
    }
  }
  return platform::wire::serialize_byte_align(stream);
}

bool validate_packet(const VehicleStateBatchPacket& packet);

VehicleStateBatchPacket to_vehicle_state_batch_packet(const core::TrafficSnapshot& snapshot);
void from_packet(const VehicleStateBatchPacket& packet, core::TrafficSnapshot& snapshot);

}  // namespace multiplayer::jak2::wire
