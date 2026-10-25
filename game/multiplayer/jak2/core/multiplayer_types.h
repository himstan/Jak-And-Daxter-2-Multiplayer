#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "game/multiplayer/jak2/core/player_data.h"
#include "game/multiplayer/jak2/player_skin.h"
#include "game/multiplayer/platform/core/player_id.h"
#include "game/multiplayer/platform/core/types.h"
#include "game/multiplayer/platform/session/session_state.h"

namespace multiplayer::jak2::core {

using PlayerSlot = platform::PlayerSlot;
using SourceContext = platform::MessageOrigin;
using EntityId = uint32_t;
using Sequence = uint32_t;

inline constexpr size_t kPlayerNameSize = 16;
inline constexpr size_t kPlayerSkinSlotCount = kMPPlayerSkinSlotCount;

inline constexpr uint8_t kMaxPlayers = 8;
inline constexpr size_t kMaxAirlockStatesPerSnapshot = 4;
inline constexpr size_t kMaxEnemies = 128;
inline constexpr size_t kMaxReplicatedEnemies = kMaxEnemies * kMaxPlayers;
inline constexpr size_t kMaxBootstrapAids = 4096;
inline constexpr size_t kGameTaskCount = 110;
inline constexpr size_t kGameTaskCompletionBytes = (kGameTaskCount + 7) / 8;
inline constexpr uint16_t kDefaultRespawnDelaySeconds = 20;
inline constexpr size_t kMaxPedestrians = 128;
inline constexpr size_t kMaxVehicles = 64;
inline constexpr PlayerSlot kInvalidPlayerSlot = platform::kInvalidPlayerSlot;
enum class PlayerActivity : uint8_t {
  UNAVAILABLE,
  LOBBY,
  GAME_STARTING,
  IN_GAME,
};

inline constexpr uint32_t kTrafficNetIdClassMask = 0xf0000000u;
inline constexpr uint32_t kTrafficNetIdOriginMask = 0x0f000000u;
inline constexpr uint32_t kTrafficNetIdSequenceMask = 0x00ffffffu;
inline constexpr uint32_t kTrafficNetIdOriginShift = 24u;
inline constexpr uint32_t kTrafficPedestrianNetIdClass = 0x10000000u;
inline constexpr uint32_t kTrafficVehicleNetIdClass = 0x20000000u;
inline constexpr uint32_t kPlayerVehicleNetIdClass = 0x40000000u;
inline constexpr uint32_t kFixedTrafficNetIdClass = 0x70000000u;
inline constexpr uint32_t kFixedTrafficNetIdNamespaceMask = 0xff000000u;
inline constexpr uint32_t kMissionVehicleNetIdNamespace = 0x70000000u;
inline constexpr uint32_t kShuttlePedestrianNetIdNamespace = 0x71000000u;
inline constexpr uint32_t kTrafficVehicleCivilianRiderId = 0xfffffffeu;

using platform::PlayerCharacter;
using platform::SessionRole;
using platform::SessionState;
using platform::SessionStatus;

using PlayerSkin = MPPlayerSkin;

struct PlayerIdentity {
  PlayerSlot player_slot = kInvalidPlayerSlot;
  platform::PlayerId player_id;
  PlayerCharacter character = PlayerCharacter::UNKNOWN;
  bool identity_ready = false;
  bool state_ready = false;
  bool joined = false;
  bool spectator_only = false;
  bool lobby_ready = false;
  std::array<uint8_t, kPlayerNameSize> name = {};
  PlayerSkin skin = {};
};

enum class PlayerLevelDisplayMode : uint8_t {
  NONE = 0,
  DISPLAY = 1,
  SPECIAL = 2,
  DISPLAY_NO_WAIT = 3,
};

inline constexpr size_t kPlayerLevelSlotCount = 6;

enum PlayerLevelFlags : uint8_t {
  LEVEL_MODE_MASK = 0x03,
  LEVEL_FORCE_VIS = 0x04,
  LEVEL_FORCE_INSIDE = 0x08,
  LEVEL_BORROW_OWNER_MASK = 0x70,
  LEVEL_BORROW_OWNER_SHIFT = 4,
  LEVEL_BORROW_SLOT = 0x80,
};

struct PlayerLevelSlot {
  uint8_t level_id = 0;
  uint8_t flags = 0;
};
static_assert(sizeof(PlayerLevelSlot) == 2);

using PlayerLevelState = std::array<PlayerLevelSlot, kPlayerLevelSlotCount>;
static_assert(sizeof(PlayerLevelState) == 12);

constexpr int player_level_borrow_owner_index(const PlayerLevelSlot& level) {
  const auto encoded =
      static_cast<uint8_t>((level.flags & LEVEL_BORROW_OWNER_MASK) >> LEVEL_BORROW_OWNER_SHIFT);
  return encoded == 0 ? -1 : static_cast<int>(encoded) - 1;
}

constexpr uint8_t player_level_borrow_slot(const PlayerLevelSlot& level) {
  return (level.flags & LEVEL_BORROW_SLOT) != 0 ? 1 : 0;
}

inline bool valid_player_level_state(const PlayerLevelState& levels, uint8_t primary_slot) {
  if (primary_slot >= levels.size() || (primary_slot != 0 && levels[primary_slot].level_id == 0)) {
    return false;
  }
  bool saw_empty = false;
  for (size_t i = 0; i < levels.size(); ++i) {
    const auto& level = levels[i];
    if (level.level_id == 0) {
      if (level.flags != 0) {
        return false;
      }
      saw_empty = true;
      continue;
    }
    if (saw_empty) {
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (levels[j].level_id == level.level_id) {
        return false;
      }
    }

    const auto encoded_owner =
        static_cast<uint8_t>((level.flags & LEVEL_BORROW_OWNER_MASK) >> LEVEL_BORROW_OWNER_SHIFT);
    if (encoded_owner == 7) {
      return false;
    }
    if (encoded_owner == 0) {
      if ((level.flags & LEVEL_BORROW_SLOT) != 0) {
        return false;
      }
      continue;
    }

    if ((level.flags & (LEVEL_FORCE_VIS | LEVEL_FORCE_INSIDE)) != 0) {
      return false;
    }
    const auto owner_index = static_cast<size_t>(encoded_owner - 1);
    if (owner_index >= levels.size() || owner_index == i || levels[owner_index].level_id == 0 ||
        player_level_borrow_owner_index(levels[owner_index]) >= 0) {
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (const auto previous_owner = player_level_borrow_owner_index(levels[j]);
          previous_owner == static_cast<int>(owner_index) &&
          player_level_borrow_slot(levels[j]) == player_level_borrow_slot(level)) {
        return false;
      }
    }
  }
  return true;
}

struct PlayerState {
  PlayerSlot player_slot = kInvalidPlayerSlot;
  PlayerActivity activity = PlayerActivity::UNAVAILABLE;
  bool identity_ready = false;
  bool state_ready = false;
  bool spectator_only = false;
  bool scene_active = false;
  bool turret_active = false;
  bool hit_invulnerable = false;
  PlayerLevelState levels{};
  uint8_t primary_level_slot = 0;
  std::array<float, 3> position = {};
  std::array<float, 3> velocity = {};
  float angle = 0.0f;
  float camera_angle_y = 0.0f;
  uint32_t state_id = 0;
  uint8_t darkjak_stage = 0;
  uint16_t buttons = 0;
  uint8_t leftx = 0;
  uint8_t lefty = 0;
  uint8_t respawn_flags = 0;
  uint8_t equipped_weapon = 0;
  uint32_t action_sequence = 0;
  uint32_t action_state_id = 0;
  uint32_t vehicle_id = 0;
  uint8_t vehicle_seat = 0;
  PlayerSlot riding_along_player_slot = kInvalidPlayerSlot;
  PlayerSlot selected_traffic_authority = kInvalidPlayerSlot;
  uint8_t mission_flags = 0;
  uint8_t visual_secrets = 0;
  uint32_t sample_time_ms = 0;
  uint32_t last_sequence = 0;
  uint64_t received_time_ms = 0;
};

constexpr uint8_t primary_level_id(const PlayerState& player) {
  return player.primary_level_slot < player.levels.size()
             ? player.levels[player.primary_level_slot].level_id
             : 0;
}

struct TrafficAuthority {
  uint32_t revision = 0;
  std::array<PlayerSlot, kMaxPlayers> assignments = [] {
    std::array<PlayerSlot, kMaxPlayers> result;
    result.fill(kInvalidPlayerSlot);
    return result;
  }();
};

struct VehicleState {
  EntityId net_id = 0;
  uint8_t vehicle_type = 0;
  uint8_t color_index = 0;
  uint8_t state_id = 0;
  PlayerSlot target_player_slot = kInvalidPlayerSlot;
  uint8_t state_flags = 0;
  uint8_t hit_points = 0;
  uint16_t level_id = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {};
  std::array<float, 3> linear_velocity = {};
  std::array<float, 3> angular_velocity = {};
  std::array<uint32_t, 4> rider_player_slots = {kInvalidPlayerSlot, kInvalidPlayerSlot,
                                                kInvalidPlayerSlot, kInvalidPlayerSlot};
  uint32_t sample_time_ms = 0;
  uint32_t last_sequence = 0;
  uint64_t received_time_ms = 0;
  bool operator==(const VehicleState&) const = default;
};

struct PlayerVehicleState {
  PlayerSlot player_slot = kInvalidPlayerSlot;
  uint8_t seat_index = 0;
  VehicleState vehicle = {};
};

struct TurretState {
  PlayerSlot player_slot = kInvalidPlayerSlot;
  uint32_t turret_aid = 0;
  float rotation_y = 0.0f;
  float rotation_x = 0.0f;
};

struct PedestrianState {
  EntityId net_id = 0;
  uint8_t object_type = 0;
  uint32_t appearance_mask = 0;
  uint8_t state_id = 0;
  int32_t hit_points = 0;
  uint8_t target_player_slot = kInvalidPlayerSlot;
  EntityId vehicle_net_id = 0;
  uint32_t transport_id = 0;
  uint8_t transport_side = 0;
  uint8_t state_flags = 0;
  uint16_t level_id = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {};
  uint32_t sample_time_ms = 0;
  uint32_t last_sequence = 0;
  uint64_t received_time_ms = 0;
  bool operator==(const PedestrianState&) const = default;
};

struct TrafficEntity {
  EntityId net_id = 0;
  PlayerSlot owner_player_slot = kInvalidPlayerSlot;
  PlayerSlot target_player_slot = kInvalidPlayerSlot;
  uint32_t level_id = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {};
  uint32_t last_sequence = 0;
};

struct TrafficSnapshot {
  enum class Kind : uint8_t {
    PEDESTRIANS,
    VEHICLES,
  };

  Kind kind = Kind::PEDESTRIANS;
  PlayerSlot source_player_slot = kInvalidPlayerSlot;
  uint32_t authority_revision = 0;
  uint32_t level_id = 0;
  uint32_t sample_time_ms = 0;
  Sequence sequence = 0;
  std::vector<PedestrianState> pedestrians;
  std::vector<VehicleState> vehicles;
  bool operator==(const TrafficSnapshot&) const = default;
};

struct EnemyState {
  EntityId actor_id = 0;
  PlayerSlot owner_player_slot = kInvalidPlayerSlot;
  PlayerSlot focus_player_slot = kInvalidPlayerSlot;
  uint32_t state_id = 0;
  int32_t hit_points = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {};
  bool attack_active = false;
  bool aggro = false;
  uint32_t sample_time_ms = 0;
  uint64_t received_time_ms = 0;
  uint32_t last_sequence = 0;
  bool operator==(const EnemyState&) const = default;
};

struct EnemySnapshot {
  PlayerSlot source_player_slot = kInvalidPlayerSlot;
  uint32_t sample_time_ms = 0;
  Sequence sequence = 0;
  std::vector<EnemyState> enemies;
  bool operator==(const EnemySnapshot&) const = default;
};

struct BossState {
  enum class Kind : uint8_t {
    PALACE_SQUID,
    WIDOW,
  };

  Kind kind = Kind::PALACE_SQUID;
  uint32_t active = 0;
  uint32_t state_id = 0;
  int32_t stage = 0;
  int32_t hit_points = 0;
  float shield_hit_points = 0.0f;
  PlayerSlot target_player_slot = kInvalidPlayerSlot;
  uint32_t draw_force_fade = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {};
  std::array<float, 3> root_position = {};
  std::array<float, 4> root_quaternion = {};
  std::array<float, 3> trajectory_source = {};
  std::array<float, 3> trajectory_destination = {};
  float trajectory_duration = 0.0f;
  int32_t trajectory_age = 0;
  uint32_t action_sequence = 0;
  uint32_t sample_time_ms = 0;
  uint64_t received_time_ms = 0;
  Sequence sequence = 0;
};

struct AirlockState {
  uint32_t airlock_aid = 0;
  uint32_t state_id = 0;
  uint32_t level_id = 0;
  Sequence sequence = 0;
};

struct AirlockSnapshot {
  PlayerSlot source_player_slot = kInvalidPlayerSlot;
  Sequence sequence = 0;
  uint64_t received_time_ms = 0;
  std::vector<AirlockState> states;
};

struct PlayerRulesState {
  uint16_t respawn_delay_seconds = kDefaultRespawnDelaySeconds;
  bool player_collision = false;
  bool friendly_fire = false;
  bool operator==(const PlayerRulesState&) const = default;
};

struct WorldState {
  Sequence sequence = 0;
  float money = 0.0f;
  float gems = 0.0f;
  float skill = 0.0f;
  uint64_t clock = 0;
  uint64_t time_of_day_frame = 0;
  float time_of_day_ratio = 0.0f;
  float weather_cloud = 0.0f;
  float weather_fog = 0.0f;
  float weather_rain = 0.0f;
  uint16_t respawn_delay_seconds = kDefaultRespawnDelaySeconds;
  bool player_collision = false;
  bool friendly_fire = false;
  std::array<uint8_t, 64> task_mask = {};
  std::array<uint8_t, 64> active_task_mask = {};
  std::array<uint8_t, kGameTaskCompletionBytes> completed_task_mask = {};
};

enum class GungamePhase : uint8_t { INACTIVE, RED_INTRO, YELLOW_INTRO, COURSE, FINISHED };
enum class GungameTargetState : uint8_t { NOT_SPAWNED, SPAWNED, BROKEN };

struct GungameTargetRecord {
  int32_t spawn_time = 0;
  GungameTargetState state = GungameTargetState::NOT_SPAWNED;
  bool operator==(const GungameTargetRecord&) const = default;
};

struct GungameState {
  Sequence sequence = 0;
  uint32_t run_id = 0;
  int32_t score = 0;
  int32_t elapsed_time = 0;
  uint8_t course_id = 0;
  GungamePhase phase = GungamePhase::INACTIVE;
  uint8_t red_intro_step = 0;
  uint8_t yellow_intro_step = 0;
  uint8_t end_door = 0;
  bool open_end = false;
  std::vector<GungameTargetRecord> targets;
};

struct BootstrapState {
  std::optional<PlayerData> player_data;
  WorldState world = {};
  uint32_t host_task = 0;
  std::array<uint8_t, 32> host_continue = {};
  std::array<float, 3> host_spawn_position = {};
  float host_spawn_angle = 0.0f;
  float host_camera_angle_y = 0.0f;
  uint32_t synchronized_aid_count = 0;
  std::array<uint32_t, kMaxBootstrapAids> synchronized_aids = {};
  Sequence sequence = 0;
};

struct GameEvent {
  uint8_t event_id = 0;
  PlayerSlot source_player_slot = kInvalidPlayerSlot;
  uint8_t payload_size = 0;
  std::array<uint8_t, 64> payload = {};
};

struct GameEventBatch {
  std::vector<GameEvent> events;
};

}  // namespace multiplayer::jak2::core
