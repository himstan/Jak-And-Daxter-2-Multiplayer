#pragma once

#include <array>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/jak2/wire/wire_policy.h"
#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/quantization.h"

namespace multiplayer::jak2::wire {

inline constexpr size_t kPlayerVehicleStatePacketWireSize = 53;

struct PlayerVehicleStateRecord {
  uint32_t sample_time_ms = 0;
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
  std::array<uint8_t, 4> rider_player_ids = {};
};

struct PlayerVehicleStatePacket
    : platform::wire::Packet<PlayerVehicleStatePacket, PacketType::PLAYER_VEHICLE_STATE> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "PLAYER_VEHICLE_STATE",
      .priority = platform::MessagePriority::CRITICAL,
      .cadence = platform::CadenceMode::PERIODIC,
      .interval_ms = 33,
      .maximum_payload_bytes = kPlayerVehicleStatePacketWireSize};

  PlayerVehicleStateRecord vehicle = {};
};

template <typename Stream, typename RecordT>
bool serialize_player_vehicle_state_record(Stream& stream, RecordT&& vehicle) {
  if constexpr (Stream::IsReading) {
    vehicle = {};
  }
  if (!platform::wire::serialize_u32(stream, vehicle.sample_time_ms) ||
      !platform::wire::serialize_u32(stream, vehicle.net_id) ||
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
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, PlayerVehicleStatePacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  }
  return serialize_player_vehicle_state_record(stream, packet.vehicle) &&
         platform::wire::serialize_byte_align(stream);
}

bool validate_packet(const PlayerVehicleStatePacket& packet);

PlayerVehicleStatePacket to_packet(const core::PlayerVehicleState& state);
void from_packet(const PlayerVehicleStatePacket& packet, core::PlayerVehicleState& state);

}  // namespace multiplayer::jak2::wire
