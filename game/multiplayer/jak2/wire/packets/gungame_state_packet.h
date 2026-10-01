#pragma once

#include "game/multiplayer/jak2/core/validation.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/quantization.h"

namespace multiplayer::jak2::wire {

inline constexpr size_t kGungameStatePacketPrefixWireSize = 20;

struct GungameStatePacket : platform::wire::Packet<GungameStatePacket, PacketType::GUNGAME_STATE> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "GUNGAME_STATE",
      .direction = platform::MessageDirection::HOST_TO_CLIENT,
      .cadence = platform::CadenceMode::PERIODIC,
      .interval_ms = 250,
      .maximum_payload_bytes = 32768};

  core::GungameState state;
};

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, GungameStatePacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading)
    packet = {};
  auto& state = packet.state;
  uint16_t count = static_cast<uint16_t>(state.targets.size());
  auto phase = static_cast<uint8_t>(state.phase);
  uint8_t open_end = state.open_end;
  if (!platform::wire::serialize_u32(stream, state.run_id) ||
      !platform::wire::serialization::serialize_i32(stream, state.score) ||
      !platform::wire::serialization::serialize_i32(stream, state.elapsed_time) ||
      !platform::wire::serialize_uint_bits(stream, count, 16) ||
      !platform::wire::serialize_uint_bits(stream, state.course_id, 8) ||
      !platform::wire::serialize_uint_bits(stream, phase, 8) ||
      !platform::wire::serialize_uint_bits(stream, state.red_intro_step, 8) ||
      !platform::wire::serialize_uint_bits(stream, state.yellow_intro_step, 8) ||
      !platform::wire::serialize_uint_bits(stream, state.end_door, 8) ||
      !platform::wire::serialize_uint_bits(stream, open_end, 8) || open_end > 1)
    return false;
  if (kGungameStatePacketPrefixWireSize + (count + 3u) / 4u + 4u * count >
      GungameStatePacket::kPolicy.maximum_payload_bytes)
    return false;
  state.phase = static_cast<core::GungamePhase>(phase);
  state.open_end = open_end != 0;
  for (uint32_t index = 0; index < count; ++index) {
    uint8_t value = 0;
    if constexpr (!Stream::IsReading)
      value = static_cast<uint8_t>(state.targets[index].state);
    if (!platform::wire::serialize_uint_bits(stream, value, 2) || value > 2)
      return false;
    if constexpr (Stream::IsReading)
      state.targets.push_back({.state = static_cast<core::GungameTargetState>(value)});
  }
  if (!platform::wire::serialize_byte_align(stream))
    return false;
  for (auto& target : state.targets)
    if (!platform::wire::serialization::serialize_i32(stream, target.spawn_time))
      return false;
  return true;
}

inline bool validate_packet(const GungameStatePacket& packet) {
  return core::valid_gungame_state(packet.state);
}

inline GungameStatePacket to_packet(const core::GungameState& state) {
  GungameStatePacket packet;
  packet.state = state;
  packet.state.sequence = 0;
  return packet;
}

inline void from_packet(const GungameStatePacket& packet, core::GungameState& state) {
  state = packet.state;
}

}  // namespace multiplayer::jak2::wire
