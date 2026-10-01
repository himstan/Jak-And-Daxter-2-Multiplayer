#pragma once

#include <cstddef>
#include <cstdint>

#include "game/multiplayer/jak2/bridge/goal_airlock_types.h"
#include "game/multiplayer/jak2/bridge/goal_boss_types.h"
#include "game/multiplayer/jak2/bridge/goal_enemy_types.h"
#include "game/multiplayer/jak2/bridge/goal_event_types.h"
#include "game/multiplayer/jak2/bridge/goal_player_types.h"
#include "game/multiplayer/jak2/bridge/goal_traffic_types.h"
#include "game/multiplayer/jak2/bridge/goal_world_types.h"
#include "game/multiplayer/jak2/core/multiplayer_types.h"

inline constexpr size_t kMPReplicationEnemyCapacity =
    multiplayer::jak2::core::kMaxEnemies * (multiplayer::jak2::core::kMaxPlayers - 1);
inline constexpr size_t kMPReplicationPedestrianCapacity = 128;
inline constexpr size_t kMPReplicationVehicleCapacity = 64;
inline constexpr size_t kMPReplicationAirlockCapacity =
    multiplayer::jak2::core::kMaxPlayers * multiplayer::jak2::core::kMaxAirlockStatesPerSnapshot;
inline constexpr size_t kMPReplicationEventCapacity = 64;
inline constexpr uint32_t kMPReplicationStateSize = 195280;

#pragma pack(push, 1)
struct MPReplicationPlayerVehicleGOAL {
  MPReplicationVehicleStateGOAL state;
  uint32_t vehicle_id;
  float turret_roty;
  float turret_rotx;
  uint8_t seat_index;
  uint8_t turret_active;
};
#pragma pack(pop)
static_assert(sizeof(MPReplicationPlayerVehicleGOAL) == 94);

struct alignas(16) MPReplicationPlayerGOAL {
  alignas(16) float presentation_position[4];
  alignas(16) float presentation_quaternion[4];
  alignas(16) float presentation_velocity[4];
  alignas(16) MPReplicationPlayerIdentityGOAL identity;
  alignas(16) MPReplicationPlayerVehicleGOAL vehicle;
  alignas(16) MPReplicationPlayerTransformGOAL transform;
  alignas(16) MPReplicationPlayerActionGOAL action;
  alignas(16) MPReplicationPlayerInputGOAL input;
  uint32_t generation;
  uint32_t lifecycle_generation;
  uint8_t activity;
  uint8_t valid;
  uint8_t mission_flags;
};
static_assert(sizeof(MPReplicationPlayerGOAL) == 544);
static_assert(offsetof(MPReplicationPlayerGOAL, identity) == 48);
static_assert(offsetof(MPReplicationPlayerGOAL, vehicle) == 336);
static_assert(offsetof(MPReplicationPlayerGOAL, transform) == 432);
static_assert(offsetof(MPReplicationPlayerGOAL, action) == 480);
static_assert(offsetof(MPReplicationPlayerGOAL, input) == 512);
static_assert(offsetof(MPReplicationPlayerGOAL, generation) == 524);
static_assert(offsetof(MPReplicationPlayerGOAL, lifecycle_generation) == 528);

struct alignas(16) MPReplicationPedestrianSlotGOAL {
  MPReplicationPedestrianStateGOAL value;
};
struct alignas(16) MPReplicationAirlockSlotGOAL {
  MPReplicationAirlockStateGOAL value;
};
static_assert(sizeof(MPReplicationPedestrianSlotGOAL) == 64);
static_assert(sizeof(MPReplicationAirlockSlotGOAL) == 16);

struct alignas(16) MPReplicationEnemySetGOAL {
  uint32_t generation;
  uint8_t valid;
  uint16_t count;
  alignas(16) MPReplicationEnemyStateGOAL enemies[kMPReplicationEnemyCapacity];
};
static_assert(sizeof(MPReplicationEnemySetGOAL) == 57360);
static_assert(offsetof(MPReplicationEnemySetGOAL, enemies) == 16);

struct alignas(16) MPReplicationTrafficSetGOAL {
  uint32_t generation;
  uint8_t valid;
  uint8_t pedestrian_count;
  uint8_t vehicle_count;
  alignas(16) MPReplicationPedestrianSlotGOAL pedestrians[kMPReplicationPedestrianCapacity];
  alignas(16) MPReplicationVehicleStateGOAL vehicles[kMPReplicationVehicleCapacity];
};
static_assert(sizeof(MPReplicationTrafficSetGOAL) == 13328);
static_assert(offsetof(MPReplicationTrafficSetGOAL, pedestrians) == 16);
static_assert(offsetof(MPReplicationTrafficSetGOAL, vehicles) == 8208);

struct alignas(16) MPReplicationBossSetGOAL {
  uint32_t generation;
  uint8_t squid_valid;
  uint8_t widow_valid;
  alignas(16) MPReplicationPalaceSquidStateGOAL squid;
  alignas(16) MPReplicationWidowStateGOAL widow;
};
static_assert(sizeof(MPReplicationBossSetGOAL) == 192);
static_assert(offsetof(MPReplicationBossSetGOAL, squid) == 16);
static_assert(offsetof(MPReplicationBossSetGOAL, widow) == 144);

struct alignas(16) MPReplicationAirlockSetGOAL {
  uint32_t generation;
  uint32_t sequence;
  uint8_t valid;
  uint8_t count;
  alignas(16) MPReplicationAirlockSlotGOAL states[kMPReplicationAirlockCapacity];
};
static_assert(sizeof(MPReplicationAirlockSetGOAL) == 528);
static_assert(offsetof(MPReplicationAirlockSetGOAL, states) == 16);

#pragma pack(push, 1)
struct MPReplicationAuthorityGOAL {
  uint32_t generation;
  uint32_t revision;
  uint8_t valid;
  uint8_t selected;
  uint8_t assignments[kMPMaxPlayers];
};
#pragma pack(pop)
static_assert(sizeof(MPReplicationAuthorityGOAL) == 18);

struct GungameTargetRecordGOAL {
  int32_t spawn_time;
  uint8_t state;
};
static_assert(sizeof(GungameTargetRecordGOAL) == 8);

#pragma pack(push, 1)
struct GungameStateGOAL {
  uint32_t sequence;
  uint32_t run_id;
  int32_t score;
  int32_t elapsed_time;
  uint32_t targets;
  uint16_t count;
  uint16_t capacity;
  uint8_t course_id;
  uint8_t phase;
  uint8_t red_intro_step;
  uint8_t yellow_intro_step;
  uint8_t end_door;
  uint8_t open_end;
};
#pragma pack(pop)
static_assert(sizeof(GungameStateGOAL) == 30);

struct alignas(16) MPReplicationFrameGOAL {
  uint32_t generation;
  alignas(16) MPReplicationPlayerGOAL players[kMPMaxPlayers];
  alignas(16) MPWorldSyncStateGOAL world;
  alignas(16) MPReplicationBootstrapStateGOAL bootstrap;
  alignas(16) MPReplicationEnemySetGOAL enemies;
  alignas(16) MPReplicationTrafficSetGOAL traffic;
  alignas(16) MPReplicationBossSetGOAL bosses;
  alignas(16) MPReplicationAirlockSetGOAL airlocks;
  alignas(16) GungameStateGOAL gungame;
  alignas(16) MPReplicationAuthorityGOAL authority;
  uint32_t world_generation;
  uint32_t bootstrap_generation;
  uint8_t world_valid;
  uint8_t bootstrap_valid;
  uint32_t identity_generation;
  uint32_t enemy_clear_generation;
  uint32_t traffic_clear_generation;
  uint32_t boss_clear_generation;
};
static_assert(sizeof(MPReplicationFrameGOAL) == 92496);
static_assert(offsetof(MPReplicationFrameGOAL, players) == 16);
static_assert(offsetof(MPReplicationFrameGOAL, world) == 4368);
static_assert(offsetof(MPReplicationFrameGOAL, bootstrap) == 4544);
static_assert(offsetof(MPReplicationFrameGOAL, enemies) == 21008);
static_assert(offsetof(MPReplicationFrameGOAL, traffic) == 78368);
static_assert(offsetof(MPReplicationFrameGOAL, bosses) == 91696);
static_assert(offsetof(MPReplicationFrameGOAL, airlocks) == 91888);
static_assert(offsetof(MPReplicationFrameGOAL, authority) == 92448);

struct alignas(16) MPReplicationStateGOAL {
  uint32_t abi_size;
  uint8_t local_player_id;
  uint8_t host_player_id;
  alignas(16) MPReplicationFrameGOAL local;
  alignas(16) MPReplicationFrameGOAL remote;
  uint8_t outbound_event_count;
  alignas(16) MPEventGOAL outbound_events[kMPReplicationEventCapacity];
  uint8_t inbound_event_count;
  alignas(16) MPEventGOAL inbound_events[kMPReplicationEventCapacity];
};

static_assert(sizeof(MPReplicationStateGOAL) == kMPReplicationStateSize);
static_assert(offsetof(MPReplicationStateGOAL, local) == 16);
static_assert(offsetof(MPReplicationStateGOAL, remote) == 92512);
static_assert(offsetof(MPReplicationStateGOAL, outbound_event_count) == 185008);
static_assert(offsetof(MPReplicationStateGOAL, outbound_events) == 185024);
static_assert(offsetof(MPReplicationStateGOAL, inbound_event_count) == 190144);
static_assert(offsetof(MPReplicationStateGOAL, inbound_events) == 190160);
