#pragma once

#include <cstdint>

#pragma pack(push, 1)
struct MPReplicationEnemyStateGOAL {
  float position[4];
  float quaternion[4];
  float presentation_velocity[4];
  uint32_t actor_id;
  int32_t hp;
  uint32_t state;
  uint8_t focus_player_id;
  uint8_t attack_flag;
  uint8_t owner_player_id;
  uint8_t is_aggro;
};

struct MPReplicationPedestrianStateGOAL {
  float position[4];
  float quaternion[4];
  uint32_t net_id;
  int32_t hp;
  uint32_t animation_profile;
  uint32_t vehicle_net_id;
  uint32_t transport_id;
  uint32_t appearance_mask;
  uint16_t level_id;
  uint8_t object_type;
  uint8_t state_id;
  uint8_t flags;
  uint8_t target_player_id;
  uint8_t transport_side;
};
#pragma pack(pop)

static_assert(sizeof(MPReplicationEnemyStateGOAL) == 64);
static_assert(sizeof(MPReplicationPedestrianStateGOAL) == 63);
