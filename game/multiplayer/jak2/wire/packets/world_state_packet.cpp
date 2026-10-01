#include "game/multiplayer/jak2/wire/packets/world_state_packet.h"

namespace multiplayer::jak2::wire {

bool validate_packet(const WorldStatePacket& packet) {
  return platform::wire::valid_unit(packet.time_of_day_ratio) &&
         platform::wire::valid_unit(packet.weather_cloud) &&
         platform::wire::valid_unit(packet.weather_fog) &&
         platform::wire::valid_unit(packet.weather_rain);
}

WorldStatePacket to_packet(const core::WorldState& state) {
  return {.clock = state.clock,
          .time_of_day_frame = state.time_of_day_frame,
          .time_of_day_ratio = state.time_of_day_ratio,
          .weather_cloud = state.weather_cloud,
          .weather_fog = state.weather_fog,
          .weather_rain = state.weather_rain,
          .task_mask = state.task_mask,
          .active_task_mask = state.active_task_mask};
}

void from_packet(const WorldStatePacket& packet, core::WorldState& state) {
  state = {};
  state.clock = packet.clock;
  state.time_of_day_frame = packet.time_of_day_frame;
  state.time_of_day_ratio = packet.time_of_day_ratio;
  state.weather_cloud = packet.weather_cloud;
  state.weather_fog = packet.weather_fog;
  state.weather_rain = packet.weather_rain;
  state.task_mask = packet.task_mask;
  state.active_task_mask = packet.active_task_mask;
}

}  // namespace multiplayer::jak2::wire
