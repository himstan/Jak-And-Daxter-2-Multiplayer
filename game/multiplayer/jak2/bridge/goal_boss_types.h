#pragma once

#include <cstdint>

#pragma pack(push, 1)
struct MPReplicationBossStateGOAL {
  float position[3];
  float quaternion[4];
  float anchor_position[3];
  float anchor_quaternion[4];
  float trajectory_source[3];
  float trajectory_destination[3];
  float trajectory_duration;
  int32_t trajectory_age;
  uint32_t state_entry;
  float health;
  float shield;
  uint8_t kind;
  uint8_t state_id;
  uint8_t stage;
  uint8_t target_player_slot;
  uint8_t draw_force_fade;
};
#pragma pack(pop)

static_assert(sizeof(MPReplicationBossStateGOAL) == 105);
