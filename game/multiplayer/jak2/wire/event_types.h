#pragma once

#include <array>
#include <cstdint>

namespace multiplayer::jak2::wire {

struct EventDescriptor {
  uint8_t id;
  const char* name;
  uint8_t payload_size;
};

inline constexpr std::array<EventDescriptor, 57> kEvents = {{
    {1, "ORB", 12},
    {2, "GEM", 12},
    {3, "BREAK", 4},
    {4, "ATTACK_ENEMY", 63},
    {5, "ENEMY_DEATH", 4},
    {6, "SCENE_START", 64},
    {7, "SCENE_REQUEST_SKIP", 64},
    {8, "SCENE_SKIPPED", 64},
    {9, "ENEMY_AUTHORITY_REQUEST", 17},
    {10, "ENEMY_AUTHORITY_ACK", 5},
    {11, "ATTACK_PED", 64},
    {12, "CHANGE_MODE_DARKJAK", 0},
    {13, "END_MODE_DARKJAK", 0},
    {14, "EDGE_GRAB", 62},
    {15, "PILOT_EDGE_GRAB", 62},
    {16, "PLAYER_RESPAWNED", 0},
    {17, "PLAYER_DIED", 0},
    {18, "CONTINUE_POINT", 32},
    {19, "WAYPOINT_BUMP", 39},
    {20, "WAYPOINT_SNAPSHOT", 39},
    {21, "WAYPOINT_REQUEST", 39},
    {22, "POLE_GRAB", 4},
    {23, "BATTLE_EVENT", 55},
    {24, "PLAYER_GUN_SHOT", 9},
    {25, "WARP_GATE", 62},
    {26, "ENEMY_GEM", 13},
    {27, "MTN_PLAT_SHOOT", 20},
    {28, "MTN_DICE", 37},
    {29, "PALACE_SQUID", 46},
    {30, "FORT_MISSILE_TARGET", 1},
    {31, "WIDOW_SPYDROID", 53},
    {32, "WIDOW_BOMB", 17},
    {33, "WIDOW_BOMB_KICK", 37},
    {34, "WIDOW_BOMB_FIZZLE", 9},
    {35, "WIDOW_BOMB_HIT", 17},
    {36, "WIDOW_CATWALK_HIT", 1},
    {37, "MISSION_LEAVE", 32},
    {38, "MISSION_PLAY_SCENE", 32},
    {39, "MISSION_FAILED_RETRY_YES", 0},
    {40, "MISSION_FAILED_RETRY_NO", 0},
    {41, "MISSION_FORTRESS_ROBOTANK_TRIGGER", 52},
    {44, "MISSION_GUNGAME_TARGET_BROKEN", 8},
    {47, "MISSION_STADIUM_BOARD1_CHALLENGE_START", 4},
    {48, "MISSION_STADIUM_BOARD1_SCORE_UPDATE", 4},
    {49, "MISSION_STADIUM_BOARD1_CHALLENGE_END", 4},
    {50, "MISSION_STADIUM_BOARD1_BOARD_PICKED_UP", 4},
    {51, "MISSION_KREW_COLLECTION_PICKUP", 1},
    {52, "MISSION_CITY_SHUTTLE_STATE", 28},
    {53, "MISSION_CITY_POWER_TURRET_STATE", 18},
    {54, "MISSION_TOMB_BOULDER_COMPLETE", 0},
    {55, "MISSION_TOMB_DAXTER_WAIT_START", 0},
    {56, "MISSION_TOMB_DAXTER_WAIT_END", 0},
    {57, "RACE", 31},
    {58, "PARTY_WIPE", 0},
    {59, "HOVER_ENEMY_SPAWN", 12},
    {60, "DRILL_EGG", 4},
    {61, "MISSION_MOUNTAIN_RESOLUTION", 1},
}};

inline const EventDescriptor* event_descriptor(uint8_t id) {
  for (const auto& event : kEvents)
    if (event.id == id)
      return &event;
  return nullptr;
}

}  // namespace multiplayer::jak2::wire
