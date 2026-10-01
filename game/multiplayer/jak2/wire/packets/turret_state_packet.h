#pragma once

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/quantization.h"

namespace multiplayer::jak2::wire {

inline constexpr size_t kTurretStatePacketWireSize = 8;

struct TurretStatePacket : platform::wire::Packet<TurretStatePacket, PacketType::TURRET_STATE> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "TURRET_STATE",
      .priority = platform::MessagePriority::CRITICAL,
      .cadence = platform::CadenceMode::PERIODIC,
      .interval_ms = 33,
      .maximum_payload_bytes = 8};

  uint32_t turret_aid = 0;
  float rotation_y = 0.0f;
  float rotation_x = 0.0f;
};

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, TurretStatePacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  }
  if (!platform::wire::serialize_u32(stream, packet.turret_aid) ||
      !platform::wire::serialize_angle(stream, packet.rotation_y) ||
      !platform::wire::serialize_angle(stream, packet.rotation_x) ||
      !platform::wire::serialize_byte_align(stream)) {
    return false;
  }
  return true;
}

bool validate_packet(const TurretStatePacket& packet);

TurretStatePacket to_packet(const core::TurretState& state);
void from_packet(const TurretStatePacket& packet, core::TurretState& state);

}  // namespace multiplayer::jak2::wire
