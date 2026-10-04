#pragma once

#include <cstdint>

#include "game/multiplayer/jak2/core/multiplayer_types.h"

#pragma pack(push, 1)
struct MPWorldSyncStateGOAL {
  float money;
  float gems;
  float skill;
  uint32_t sequence;
  uint64_t clock;
  uint64_t time_of_day_frame;
  float time_of_day_ratio;
  float weather_cloud;
  float weather_fog;
  float weather_rain;
  uint16_t respawn_delay_seconds;
  uint8_t player_collision;
  uint8_t friendly_fire;
  uint8_t task_mask[64];
  uint8_t active_task_mask[64];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MPReplicationBootstrapStateGOAL {
  float host_spawn_position[4];
  uint32_t synchronized_aids[multiplayer::jak2::core::kMaxBootstrapAids];
  uint8_t host_continue[32];
  uint32_t phase;
  uint32_t sequence;
  uint32_t host_task;
  float host_spawn_angle;
  float host_camera_angle_y;
  uint16_t synchronized_aid_count;
};
#pragma pack(pop)

static_assert(sizeof(MPWorldSyncStateGOAL) == 180);
static_assert(sizeof(MPReplicationBootstrapStateGOAL) == 16454);
