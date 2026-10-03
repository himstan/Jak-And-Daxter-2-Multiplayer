#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "game/multiplayer/jak2/player_appearance.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"

struct MPPlayerAppearanceGOAL {
  uint32_t colors[kMPPlayerAppearanceSlotCount];
  float strengths[kMPPlayerAppearanceSlotCount];
};
static_assert(sizeof(MPPlayerAppearanceGOAL) == 256);

inline MPPlayerAppearance get_player_appearance_from_goal(const MPPlayerAppearanceGOAL& goal) {
  MPPlayerAppearance appearance = {};
  std::ranges::copy(goal.colors, appearance.colors.begin());
  std::ranges::copy(goal.strengths, appearance.strengths.begin());
  return appearance;
}

inline void copy_player_appearance_to_goal(const MPPlayerAppearance& appearance,
                                           MPPlayerAppearanceGOAL& goal) {
  std::ranges::copy(appearance.colors, std::begin(goal.colors));
  std::ranges::copy(appearance.strengths, std::begin(goal.strengths));
}

#pragma pack(push, 1)
struct MPReplicationPlayerIdentityGOAL {
  MPPlayerAppearanceGOAL appearance;
  uint8_t name[16];
  uint8_t player_id;
  uint8_t character;
  uint8_t identity_ready;
  uint8_t state_ready;
  uint8_t joined;
  uint8_t spectator_only;
  uint8_t lobby_ready;
};

struct MPPlayerLevelSlotGOAL {
  uint8_t level_id;
  uint8_t flags;
};

struct MPReplicationPlayerTransformGOAL {
  float position[4];
  float velocity[4];
  float angle;
  MPPlayerLevelSlotGOAL levels[multiplayer::jak2::core::kPlayerLevelSlotCount];
};

struct MPReplicationPlayerActionGOAL {
  uint32_t target_state_id;
  uint32_t authoritative_sequence;
  uint32_t action_state_id;
  uint8_t darkjak_stage;
  uint8_t scene_state;
  uint8_t respawn_flags;
  uint8_t riding_along_player_id;
  uint8_t visual_secrets;
};

struct MPReplicationPlayerInputGOAL {
  float camera_angle_y;
  uint16_t buttons;
  uint8_t leftx;
  uint8_t lefty;
  uint8_t equipped_weapon;
};
#pragma pack(pop)

static_assert(sizeof(MPReplicationPlayerIdentityGOAL) == 279);
static_assert(sizeof(MPPlayerLevelSlotGOAL) == 2);
static_assert(sizeof(MPReplicationPlayerTransformGOAL) == 48);
static_assert(sizeof(MPReplicationPlayerActionGOAL) == 17);
static_assert(sizeof(MPReplicationPlayerInputGOAL) == 9);
