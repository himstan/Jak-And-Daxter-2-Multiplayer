#pragma once

#include <cstddef>
#include <cstdint>

struct alignas(16) MPEventGOAL {
  uint8_t etype;
  uint8_t source_player_id;
  uint8_t payload_size;
  alignas(16) uint8_t data[64];
};

static_assert(sizeof(MPEventGOAL) == 80);
static_assert(offsetof(MPEventGOAL, data) == 16);
