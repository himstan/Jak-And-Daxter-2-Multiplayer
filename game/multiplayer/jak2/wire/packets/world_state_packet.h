#pragma once

#include <array>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/quantization.h"

namespace multiplayer::jak2::wire {

inline constexpr size_t kWorldStatePacketWireSize = 148;

struct WorldStatePacket : platform::wire::Packet<WorldStatePacket, PacketType::WORLD_STATE> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "WORLD_STATE",
      .direction = platform::MessageDirection::HOST_TO_CLIENT,
      .cadence = platform::CadenceMode::PERIODIC,
      .interval_ms = 100,
      .maximum_payload_bytes = kWorldStatePacketWireSize};

  uint64_t clock = 0;
  uint64_t time_of_day_frame = 0;
  float time_of_day_ratio = 0.0f;
  float weather_cloud = 0.0f;
  float weather_fog = 0.0f;
  float weather_rain = 0.0f;
  std::array<uint8_t, 64> task_mask = {};
  std::array<uint8_t, 64> active_task_mask = {};
};

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, WorldStatePacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  }
  if (!platform::wire::serialize_u64(stream, packet.clock) ||
      !platform::wire::serialize_u64(stream, packet.time_of_day_frame) ||
      !platform::wire::serialize_unit(stream, packet.time_of_day_ratio) ||
      !platform::wire::serialize_unit(stream, packet.weather_cloud) ||
      !platform::wire::serialize_unit(stream, packet.weather_fog) ||
      !platform::wire::serialize_unit(stream, packet.weather_rain) ||
      !platform::wire::serialization::serialize_raw_bytes(stream, packet.task_mask.data(),
                                                          packet.task_mask.size()) ||
      !platform::wire::serialization::serialize_raw_bytes(stream, packet.active_task_mask.data(),
                                                          packet.active_task_mask.size()) ||
      !platform::wire::serialize_byte_align(stream)) {
    return false;
  }
  return true;
}

bool validate_packet(const WorldStatePacket& packet);

WorldStatePacket to_packet(const core::WorldState& state);
void from_packet(const WorldStatePacket& packet, core::WorldState& state);

}  // namespace multiplayer::jak2::wire
