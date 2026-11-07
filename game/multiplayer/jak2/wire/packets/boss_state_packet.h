#pragma once

#include <array>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/jak2/wire/wire_policy.h"
#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/quantization.h"

namespace multiplayer::jak2::wire {

struct BossStatePacket : platform::wire::Packet<BossStatePacket, PacketType::BOSS_STATE> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "BOSS_STATE",
      .direction = platform::MessageDirection::HOST_TO_CLIENT,
      .cadence = platform::CadenceMode::PERIODIC,
      .interval_ms = 33,
      .maximum_payload_bytes = 62};

  uint32_t sample_time_ms = 0;
  uint8_t kind = 0;
  uint8_t state_id = 0;
  uint8_t stage = 0;
  uint8_t target_player_slot = kWireInvalidPlayerSlot;
  uint8_t draw_force_fade = 0;
  float health = 0.0f;
  float shield = 0.0f;
  uint32_t state_entry = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {0.0f, 0.0f, 0.0f, 1.0f};
  std::array<float, 3> anchor_position = {};
  std::array<float, 4> anchor_quaternion = {0.0f, 0.0f, 0.0f, 1.0f};
  std::array<float, 3> trajectory_source = {};
  std::array<float, 3> trajectory_destination = {};
  float trajectory_duration = 0.0f;
  int32_t trajectory_age = 0;
};

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, BossStatePacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  }
  return platform::wire::serialize_u32(stream, packet.sample_time_ms) &&
         platform::wire::serialize_uint_bits(stream, packet.kind, 2) &&
         platform::wire::serialize_uint_bits(stream, packet.state_id, 5) &&
         platform::wire::serialize_uint_bits(stream, packet.stage, 3) &&
         platform::wire::serialize_uint_bits(stream, packet.target_player_slot, 4) &&
         platform::wire::serialize_u8(stream, packet.draw_force_fade) &&
         platform::wire::serialize_unit(stream, packet.health) &&
         platform::wire::serialize_unit(stream, packet.shield) &&
         platform::wire::serialize_u32(stream, packet.state_entry) &&
         platform::wire::serialize_position_array(stream, packet.position) &&
         platform::wire::serialize_quaternion_array(stream, packet.quaternion) &&
         platform::wire::serialize_position_array(stream, packet.anchor_position) &&
         platform::wire::serialize_quaternion_array(stream, packet.anchor_quaternion) &&
         platform::wire::serialize_position_array(stream, packet.trajectory_source) &&
         platform::wire::serialize_position_array(stream, packet.trajectory_destination) &&
         platform::wire::serialize_trajectory_duration(stream, packet.trajectory_duration) &&
         platform::wire::serialization::serialize_i32(stream, packet.trajectory_age) &&
         platform::wire::serialize_byte_align(stream) &&
         valid_wire_player_reference(packet.target_player_slot);
}

bool validate_packet(const BossStatePacket& packet);
BossStatePacket to_boss_state_packet(const core::BossState& state);
void from_packet(const BossStatePacket& packet, core::BossState& state);

}  // namespace multiplayer::jak2::wire
