#include "game/multiplayer/jak2/wire/packets/bootstrap_state_packet.h"

#include <algorithm>

namespace multiplayer::jak2::wire {

bool validate_packet(const BootstrapStatePacket& packet) {
  return packet.player_collision <= 1 && packet.friendly_fire <= 1 && std::isfinite(packet.money) &&
         std::isfinite(packet.gems) && std::isfinite(packet.skill) &&
         platform::wire::valid_position_array(packet.host_spawn_position) &&
         packet.synchronized_aid_count <= core::kMaxBootstrapAids &&
         packet.synchronized_aid_count == packet.synchronized_aids.size() &&
         platform::wire::valid_unit(packet.time_of_day_ratio) &&
         platform::wire::valid_unit(packet.weather_cloud) &&
         platform::wire::valid_unit(packet.weather_fog) &&
         platform::wire::valid_unit(packet.weather_rain) &&
         platform::wire::valid_angle(packet.host_camera_angle_y);
}

BootstrapStatePacket to_packet(const core::BootstrapState& state) {
  BootstrapStatePacket packet;
  packet.money = state.world.money;
  packet.gems = state.world.gems;
  packet.skill = state.world.skill;
  packet.host_spawn_position = state.host_spawn_position;
  packet.host_task = state.host_task;
  packet.host_continue = state.host_continue;
  packet.task_mask = state.world.task_mask;
  packet.active_task_mask = state.world.active_task_mask;
  packet.synchronized_aid_count = state.synchronized_aid_count;
  const auto copied_count =
      std::min<size_t>(state.synchronized_aid_count, state.synchronized_aids.size());
  packet.synchronized_aids.assign(state.synchronized_aids.begin(),
                                  state.synchronized_aids.begin() + copied_count);
  packet.clock = state.world.clock;
  packet.time_of_day_frame = state.world.time_of_day_frame;
  packet.time_of_day_ratio = state.world.time_of_day_ratio;
  packet.weather_cloud = state.world.weather_cloud;
  packet.weather_fog = state.world.weather_fog;
  packet.weather_rain = state.world.weather_rain;
  packet.player_collision = state.world.player_collision;
  packet.friendly_fire = state.world.friendly_fire;
  packet.host_camera_angle_y = platform::wire::canonical_angle(state.host_camera_angle_y);
  return packet;
}

void from_packet(const BootstrapStatePacket& packet, core::BootstrapState& state) {
  state = {};
  state.world.money = packet.money;
  state.world.gems = packet.gems;
  state.world.skill = packet.skill;
  state.host_spawn_position = packet.host_spawn_position;
  state.host_task = packet.host_task;
  state.host_continue = packet.host_continue;
  state.world.task_mask = packet.task_mask;
  state.world.active_task_mask = packet.active_task_mask;
  state.synchronized_aid_count = static_cast<uint32_t>(packet.synchronized_aids.size());
  std::ranges::copy(packet.synchronized_aids, state.synchronized_aids.begin());
  state.world.clock = packet.clock;
  state.world.time_of_day_frame = packet.time_of_day_frame;
  state.world.time_of_day_ratio = packet.time_of_day_ratio;
  state.world.weather_cloud = packet.weather_cloud;
  state.world.weather_fog = packet.weather_fog;
  state.world.weather_rain = packet.weather_rain;
  state.world.player_collision = packet.player_collision != 0;
  state.world.friendly_fire = packet.friendly_fire != 0;
  state.host_camera_angle_y = packet.host_camera_angle_y;
}

}  // namespace multiplayer::jak2::wire
