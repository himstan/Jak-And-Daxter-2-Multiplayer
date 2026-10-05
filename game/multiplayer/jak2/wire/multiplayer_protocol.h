#pragma once

#include <array>
#include <cstdint>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/player_skin.h"

inline constexpr size_t kMultiplayerPlayerNameSize = 16;
inline constexpr uint32_t kMPMaxPlayers = 8;
inline constexpr uint8_t kMPInvalidPlayerId = 0xffu;
inline constexpr uint32_t kMPVehicleCivilianRiderId = 0xfffffffeu;

static_assert(kMPMaxPlayers >= 2 && kMPMaxPlayers <= 32,
              "multiplayer player capacity must be between 2 and 32");
static_assert(kMPMaxPlayers <= 16, "Traffic net IDs only reserve 4 bits for player origin");

enum class PacketType : uint8_t {
  PLAYER_STATE = 0,
  GAME_EVENT_BATCH = 1,
  ENEMY_STATE_BATCH = 2,
  PEDESTRIAN_STATE_BATCH = 3,
  VEHICLE_STATE_BATCH = 4,
  TURRET_STATE = 5,
  PALACE_SQUID_STATE = 6,
  AIRLOCK_STATE_BATCH = 7,
  WIDOW_STATE = 8,
  WORLD_STATE = 9,
  TRAFFIC_AUTHORITY_STATE = 10,
  PLAYER_VEHICLE_STATE = 11,
  GUNGAME_STATE = 12,
  PLAYER_RULES = 13,
  COUNT = 14
};

inline constexpr uint8_t kMPPlayerStateFlagSpectatorOnly = 1u << 0;
inline constexpr uint8_t kMPPlayerStateFlagSceneActive = 1u << 1;
inline constexpr uint8_t kMPPlayerStateFlagTurretActive = 1u << 2;
inline constexpr uint8_t kMPPlayerStateFlagRespawnShift = 3;
inline constexpr uint8_t kMPPlayerStateFlagRespawnMask = 0x07u << kMPPlayerStateFlagRespawnShift;

inline uint8_t pack_player_state_flags(const bool spectator_only,
                                       const bool scene_active,
                                       const bool turret_active,
                                       const uint8_t respawn_flags) {
  return (spectator_only ? kMPPlayerStateFlagSpectatorOnly : 0) |
         (scene_active ? kMPPlayerStateFlagSceneActive : 0) |
         (turret_active ? kMPPlayerStateFlagTurretActive : 0) |
         static_cast<uint8_t>((respawn_flags & 0x07u) << kMPPlayerStateFlagRespawnShift);
}

inline bool is_player_state_flag_spectator(const uint8_t flags) {
  return (flags & kMPPlayerStateFlagSpectatorOnly) != 0;
}

inline bool is_player_state_flag_scene_active(const uint8_t flags) {
  return (flags & kMPPlayerStateFlagSceneActive) != 0;
}

inline bool is_player_state_flag_turret_active(const uint8_t flags) {
  return (flags & kMPPlayerStateFlagTurretActive) != 0;
}

inline uint8_t has_player_state_flag_respawn_flags(const uint8_t flags) {
  return static_cast<uint8_t>((flags & kMPPlayerStateFlagRespawnMask) >>
                              kMPPlayerStateFlagRespawnShift);
}

inline constexpr size_t MAX_ENEMIES_PER_PACKET = 32;
inline constexpr size_t MAX_PEDESTRIANS_PER_PACKET = multiplayer::jak2::core::kMaxPedestrians;
inline constexpr size_t MAX_VEHICLES_PER_PACKET = multiplayer::jak2::core::kMaxVehicles;
