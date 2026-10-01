#pragma once

#include <array>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/jak2/wire/wire_policy.h"
#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/quantization.h"

namespace multiplayer::jak2::wire {

struct PalaceSquidStatePacket
    : platform::wire::Packet<PalaceSquidStatePacket, PacketType::PALACE_SQUID_STATE> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "PALACE_SQUID_STATE",
      .direction = platform::MessageDirection::HOST_TO_CLIENT,
      .cadence = platform::CadenceMode::PERIODIC,
      .interval_ms = 33,
      .maximum_payload_bytes = 69};

  uint32_t sample_time_ms = 0;
  uint32_t active = 0;
  uint32_t state_id = 0;
  int32_t stage = 0;
  int32_t hit_points = 0;
  float shield_hit_points = 0.0f;
  uint8_t target_player_id = kWireInvalidPlayerId;
  uint32_t draw_force_fade = 0;
  uint32_t action_sequence = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {};
  std::array<float, 3> root_position = {};
  std::array<float, 4> root_quaternion = {};
  std::array<float, 3> trajectory_source = {};
  std::array<float, 3> trajectory_destination = {};
  float trajectory_duration = 0.0f;
  int32_t trajectory_age = 0;
};

struct WidowStatePacket : platform::wire::Packet<WidowStatePacket, PacketType::WIDOW_STATE> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "WIDOW_STATE",
      .direction = platform::MessageDirection::HOST_TO_CLIENT,
      .cadence = platform::CadenceMode::PERIODIC,
      .interval_ms = 50,
      .maximum_payload_bytes = 19};

  uint32_t sample_time_ms = 0;
  uint32_t active = 0;
  uint32_t state_id = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {};
};

template <typename Stream>
bool serialize_boss_quaternion(Stream& stream, std::array<float, 4>& value) {
  return platform::wire::serialize_quaternion_array(stream, value);
}

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, PalaceSquidStatePacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  }
  if (!platform::wire::serialize_u32(stream, packet.sample_time_ms) ||
      !platform::wire::serialize_uint_bits(stream, packet.active, 1) ||
      !platform::wire::serialize_uint_bits(stream, packet.state_id, 4) ||
      !platform::wire::serialization::serialize_i32(stream, packet.stage) ||
      !platform::wire::serialization::serialize_i32(stream, packet.hit_points) ||
      !platform::wire::serialize_unit(stream, packet.shield_hit_points) ||
      !platform::wire::serialize_uint_bits(stream, packet.target_player_id, 4) ||
      !platform::wire::serialize_uint_bits(stream, packet.draw_force_fade, 8) ||
      !platform::wire::serialize_u32(stream, packet.action_sequence) ||
      !platform::wire::serialize_position_array(stream, packet.position) ||
      !serialize_boss_quaternion(stream, packet.quaternion) ||
      !platform::wire::serialize_position_array(stream, packet.root_position) ||
      !serialize_boss_quaternion(stream, packet.root_quaternion) ||
      !platform::wire::serialize_position_array(stream, packet.trajectory_source) ||
      !platform::wire::serialize_position_array(stream, packet.trajectory_destination) ||
      !platform::wire::serialize_trajectory_duration(stream, packet.trajectory_duration) ||
      !platform::wire::serialization::serialize_i32(stream, packet.trajectory_age) ||
      !platform::wire::serialize_byte_align(stream)) {
    return false;
  }
  return valid_wire_player_reference(packet.target_player_id);
}

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, WidowStatePacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  }
  if (!platform::wire::serialize_u32(stream, packet.sample_time_ms) ||
      !platform::wire::serialize_uint_bits(stream, packet.active, 1) ||
      !platform::wire::serialize_uint_bits(stream, packet.state_id, 5) ||
      !platform::wire::serialize_position_array(stream, packet.position) ||
      !serialize_boss_quaternion(stream, packet.quaternion) ||
      !platform::wire::serialize_byte_align(stream)) {
    return false;
  }
  return true;
}

bool validate_packet(const PalaceSquidStatePacket& packet);
bool validate_packet(const WidowStatePacket& packet);

PalaceSquidStatePacket to_palace_squid_state_packet(const core::BossState& state);
WidowStatePacket to_widow_state_packet(const core::BossState& state);
void from_packet(const PalaceSquidStatePacket& packet, core::BossState& state);
void from_packet(const WidowStatePacket& packet, core::BossState& state);

}  // namespace multiplayer::jak2::wire
