#pragma once

#include <cstdint>

struct MPReplicationPalaceSquidStateGOAL {
  uint32_t active;
  uint32_t state_id;
  int32_t stage;
  int32_t hit_points;
  float shield_hit_points;
  uint8_t target_player_id;
  uint8_t draw_force_fade;
  uint32_t action_seq;
  float x;
  float y;
  float z;
  float quat_x;
  float quat_y;
  float quat_z;
  float quat_w;
  float root_x;
  float root_y;
  float root_z;
  float root_quat_x;
  float root_quat_y;
  float root_quat_z;
  float root_quat_w;
  float traj_src_x;
  float traj_src_y;
  float traj_src_z;
  float traj_dest_x;
  float traj_dest_y;
  float traj_dest_z;
  float traj_duration;
  int32_t traj_age;
};

struct MPReplicationWidowStateGOAL {
  uint32_t active;
  uint32_t state_id;
  float x;
  float y;
  float z;
  float quat_x;
  float quat_y;
  float quat_z;
  float quat_w;
};

static_assert(sizeof(MPReplicationPalaceSquidStateGOAL) == 116);
static_assert(sizeof(MPReplicationWidowStateGOAL) == 36);
