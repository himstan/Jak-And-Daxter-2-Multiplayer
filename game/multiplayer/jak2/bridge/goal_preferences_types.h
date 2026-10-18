#pragma once

#include <cstdint>

#include "game/multiplayer/jak2/bridge/goal_player_types.h"

#pragma pack(push, 1)
struct MultiplayerPreferencesGOAL {
  MPPlayerSkinGOAL skin;
  uint32_t player_name;
  uint32_t room_code;
  uint16_t network_port;
  uint16_t respawn_delay_seconds;
  uint8_t session_player_limit;
  uint8_t preferred_character;
  uint8_t automatic_port_mapping;
  uint8_t player_collision;
  uint8_t friendly_fire;
  uint8_t nametag_visibility;
  uint8_t player_map_marker;
  uint8_t host_visibility;
  uint16_t glow_fade_in_ms;
  uint16_t glow_fade_out_ms;
  uint16_t glow_fade_distance;
  uint8_t glow_mode;
  uint8_t glow_width;
  uint8_t glow_rounding;
  uint8_t glow_blur;
};
#pragma pack(pop)

static_assert(sizeof(MultiplayerPreferencesGOAL) == 286);
