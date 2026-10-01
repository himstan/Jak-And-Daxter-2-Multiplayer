#pragma once

#include <cstdint>

struct MPReplicationVehicleStateGOAL {
  uint32_t net_id;
  uint8_t vehicle_type;
  uint8_t color_index;
  uint8_t state_id;
  uint8_t target_player_id;
  float x;
  float y;
  float z;
  float quat_x;
  float quat_y;
  float quat_z;
  float quat_w;
  float lin_vel_x;
  float lin_vel_y;
  float lin_vel_z;
  float ang_vel_x;
  float ang_vel_y;
  float ang_vel_z;
  uint8_t state_flags;
  uint8_t hit_points;
  uint16_t level_id;
  uint32_t rider_player_ids[4];
};

static_assert(sizeof(MPReplicationVehicleStateGOAL) == 80);
