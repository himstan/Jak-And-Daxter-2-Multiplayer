#pragma once

#include <cstdint>

struct PlayerDataGOAL {
  float health;
  float dark_eco;
  float ammo[4];
  uint32_t cosmetic_secrets;
  uint32_t unlocked_cosmetic_secrets;
};
static_assert(sizeof(PlayerDataGOAL) == 32);
