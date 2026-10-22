#pragma once

#include <cstdint>

struct MPReplicationAirlockStateGOAL {
  uint32_t airlock_aid;
  uint32_t sequence;
  uint16_t level_id;
  uint8_t state_id;
  uint8_t source_player_slot;
};

static_assert(sizeof(MPReplicationAirlockStateGOAL) == 12);
