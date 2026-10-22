#pragma once

#include <cstdint>

namespace multiplayer::jak2::wire {

inline constexpr uint8_t kWireInvalidPlayerSlot = 0x0fu;
inline constexpr uint8_t kWireCivilianRiderId = 0x0eu;

inline bool valid_wire_player_reference(const uint8_t value) {
  return value <= 7u || value == kWireInvalidPlayerSlot;
}

inline bool valid_wire_rider_reference(const uint8_t value) {
  return value <= 7u || value == kWireCivilianRiderId || value == kWireInvalidPlayerSlot;
}

}  // namespace multiplayer::jak2::wire
