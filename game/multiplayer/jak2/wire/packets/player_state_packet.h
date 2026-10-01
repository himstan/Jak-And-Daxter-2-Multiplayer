#pragma once

#include <array>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/jak2/wire/wire_policy.h"
#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/quantization.h"

namespace multiplayer::jak2::wire {

struct PlayerStatePacket : platform::wire::Packet<PlayerStatePacket, PacketType::PLAYER_STATE> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "PLAYER_STATE",
      .priority = platform::MessagePriority::CRITICAL,
      .cadence = platform::CadenceMode::PERIODIC,
      .interval_ms = 33,
      .maximum_payload_bytes = 49};

  uint32_t sample_time_ms = 0;
  core::PlayerActivity activity = core::PlayerActivity::UNAVAILABLE;
  std::array<float, 3> position = {};
  float angle = 0.0f;
  std::array<float, 3> velocity = {};
  uint32_t state_id = 0;
  core::PlayerLevelState levels = {};
  uint8_t darkjak_stage = 0;
  uint16_t buttons = 0;
  uint8_t leftx = 0;
  uint8_t lefty = 0;
  uint8_t flags = 0;
  float camera_angle_y = 0.0f;
  uint32_t vehicle_id = 0;
  uint8_t vehicle_seat = 0;
  uint8_t equipped_weapon = 0;
  uint32_t action_sequence = 0;
  uint32_t action_state_id = 0;
  uint8_t riding_along_player_id = kWireInvalidPlayerId;
  uint8_t mission_flags = 0;
  uint8_t visual_secrets = 0;
};

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, PlayerStatePacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  }
  if (!platform::wire::serialize_u32(stream, packet.sample_time_ms) ||
      !platform::wire::serialize_uint_bits(stream, packet.activity, 2) ||
      !platform::wire::serialize_position_array(stream, packet.position) ||
      !platform::wire::serialize_angle(stream, packet.angle) ||
      !platform::wire::serialize_linear_velocity_array(stream, packet.velocity) ||
      !platform::wire::serialize_uint_bits(stream, packet.state_id, 8)) {
    return false;
  }
  for (auto& level : packet.levels) {
    if (!platform::wire::serialize_u8(stream, level.level_id) ||
        !platform::wire::serialize_u8(stream, level.flags)) {
      return false;
    }
  }
  if (!platform::wire::serialize_u8(stream, packet.darkjak_stage) ||
      !platform::wire::serialize_u16(stream, packet.buttons) ||
      !platform::wire::serialize_u8(stream, packet.leftx) ||
      !platform::wire::serialize_u8(stream, packet.lefty) ||
      !platform::wire::serialize_uint_bits(stream, packet.flags, 6) ||
      !platform::wire::serialize_angle(stream, packet.camera_angle_y) ||
      !platform::wire::serialize_u32(stream, packet.vehicle_id) ||
      !platform::wire::serialize_uint_bits(stream, packet.vehicle_seat, 2) ||
      !platform::wire::serialize_uint_bits(stream, packet.equipped_weapon, 3) ||
      !platform::wire::serialize_uint_bits(stream, packet.action_sequence, 8) ||
      !platform::wire::serialize_uint_bits(stream, packet.action_state_id, 8) ||
      !platform::wire::serialize_uint_bits(stream, packet.riding_along_player_id, 4) ||
      !platform::wire::serialize_uint_bits(stream, packet.mission_flags, 2) ||
      !platform::wire::serialize_uint_bits(stream, packet.visual_secrets, 3) ||
      !platform::wire::serialize_byte_align(stream)) {
    return false;
  }
  return true;
}

bool validate_packet(const PlayerStatePacket& packet);

PlayerStatePacket to_packet(const core::PlayerState& state);
void from_packet(const PlayerStatePacket& packet, core::PlayerState& state);

}  // namespace multiplayer::jak2::wire
