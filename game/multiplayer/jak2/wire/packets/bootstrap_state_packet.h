#pragma once

#include <array>
#include <vector>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/quantization.h"

namespace multiplayer::jak2::wire {

struct BootstrapStatePacket : platform::wire::Packet<BootstrapStatePacket, PacketType::COUNT> {
  float money = 0.0f;
  float gems = 0.0f;
  float skill = 0.0f;
  std::array<float, 3> host_spawn_position = {};
  uint32_t host_task = 0;
  std::array<uint8_t, 32> host_continue = {};
  std::array<uint8_t, 64> task_mask = {};
  std::array<uint8_t, 64> active_task_mask = {};
  uint32_t synchronized_aid_count = 0;
  std::vector<uint32_t> synchronized_aids;
  uint64_t clock = 0;
  uint64_t time_of_day_frame = 0;
  float time_of_day_ratio = 0.0f;
  float weather_cloud = 0.0f;
  float weather_fog = 0.0f;
  float weather_rain = 0.0f;
  float host_camera_angle_y = 0.0f;
};

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, BootstrapStatePacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  }

  if (!platform::wire::serialization::serialize_f32(stream, packet.money) ||
      !platform::wire::serialization::serialize_f32(stream, packet.gems) ||
      !platform::wire::serialization::serialize_f32(stream, packet.skill) ||
      !platform::wire::serialize_position_array(stream, packet.host_spawn_position) ||
      !platform::wire::serialize_u32(stream, packet.host_task) ||
      !platform::wire::serialization::serialize_raw_bytes(stream, packet.host_continue.data(),
                                                          packet.host_continue.size()) ||
      !platform::wire::serialization::serialize_raw_bytes(stream, packet.task_mask.data(),
                                                          packet.task_mask.size()) ||
      !platform::wire::serialization::serialize_raw_bytes(stream, packet.active_task_mask.data(),
                                                          packet.active_task_mask.size())) {
    return false;
  }

  uint16_t aid_count = static_cast<uint16_t>(packet.synchronized_aids.size());
  if (!platform::wire::serialize_u16(stream, aid_count)) {
    return false;
  }
  if constexpr (Stream::IsReading) {
    if (aid_count > core::kMaxBootstrapAids) {
      return false;
    }
    packet.synchronized_aid_count = aid_count;
    packet.synchronized_aids.resize(aid_count);
  }
  for (auto& aid : packet.synchronized_aids) {
    if (!platform::wire::serialize_u32(stream, aid)) {
      return false;
    }
  }

  return platform::wire::serialize_u64(stream, packet.clock) &&
         platform::wire::serialize_u64(stream, packet.time_of_day_frame) &&
         platform::wire::serialize_unit(stream, packet.time_of_day_ratio) &&
         platform::wire::serialize_unit(stream, packet.weather_cloud) &&
         platform::wire::serialize_unit(stream, packet.weather_fog) &&
         platform::wire::serialize_unit(stream, packet.weather_rain) &&
         platform::wire::serialize_angle(stream, packet.host_camera_angle_y) &&
         platform::wire::serialize_byte_align(stream);
}

bool validate_packet(const BootstrapStatePacket& packet);

BootstrapStatePacket to_packet(const core::BootstrapState& state);
void from_packet(const BootstrapStatePacket& packet, core::BootstrapState& state);

}  // namespace multiplayer::jak2::wire
