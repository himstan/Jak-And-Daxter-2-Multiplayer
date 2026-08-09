#pragma once

#include <cstddef>
#include <cstdint>

#include "game/multiplayer/generated/multiplayer_schema_generated.h"

#pragma pack(push, 1)

const int DISCOVERY_PORT = 26211;
const char* const DISCOVERY_MAGIC = "OG_MP_DISCOVERY";
inline constexpr size_t kPacketHeaderWireSize = sizeof(uint8_t) + sizeof(uint32_t);
inline constexpr size_t kEventEnvelopeHeaderWireSize = kPacketHeaderWireSize + sizeof(uint32_t) + sizeof(uint16_t);

enum class MultiplayerChannel : uint8_t {
  STATE = 0,
  CONTROL = 1,
};

enum class MultiplayerStatus : int32_t {
  IDLE = 0,
  SEARCHING = 1,
  FOUND = 2,
  CONNECTING = 3,
  CONNECTED_LOBBY = 4,
  GAME_STARTING = 5,
  IN_GAME = 6,
  RECONNECTING = 7,
  HOST_LEFT = 8,
  FAILED = -1,
  VERSION_MISMATCH = -2,
  CREDENTIAL_DISCOVERY_FAILED = -3
};

enum class MultiplayerConnectionPhase : int32_t {
  IDLE = 0,
  VALIDATING = 1,
  CONTACTING_HOST = 2,
  AUTHENTICATING = 3,
  CONNECTED = 4,
};

enum class MultiplayerConnectionFailure : int32_t {
  NONE = 0,
  INVALID_INVITE = 1,
  HOST_UNREACHABLE = 2,
  ROOM_CODE_REJECTED = 3,
  HOST_FULL = 4,
  VERSION_MISMATCH = 5,
  LAN_TIMEOUT = 6,
  CREDENTIAL_DISCOVERY_FAILED = 7,
};

enum class MultiplayerHostSetupStatus : int32_t {
  IDLE = 0,
  STARTING = 1,
  CONFIGURING_ROUTER = 2,
  READY = 3,
  MAPPING_DISABLED = 4,
  MAPPING_FAILED = -1,
  BIND_FAILED = -2,
  START_FAILED = -3,
};

enum class MultiplayerHostCopyMode : int32_t {
  UNAVAILABLE = 0,
  INVITE = 1,
  ROOM_CODE = 2,
};

// ENet disconnect data distinguishes an intentional session shutdown from a
// transport loss. Zero remains the transient/network-loss reason used by ENet
// itself and by reconnect recovery.
inline constexpr uint32_t kDisconnectReasonHostClosed = 1;
inline constexpr uint32_t kDisconnectReasonClientClosed = 2;
inline constexpr uint32_t kDisconnectReasonHostFull = 3;
inline constexpr uint32_t kDisconnectReasonAuthenticationRejected = 4;

enum class PacketType : uint8_t {
  STATE_UPDATE = 0,
  EVENT_JOIN = 1,
  EVENT_LEAVE = 2,
  EVENT_GAME = 3,
  BOOTSTRAP = 4,
  ENEMY_SYNC = 5,
  PEDESTRIAN_SYNC = 6,
  VEHICLE_SYNC = 7,
  TURRET_SYNC = 8,
  PALACE_SQUID_SYNC = 9,
  AIRLOCK_SYNC = 10,
  WIDOW_SYNC = 11,
  COUNT = 12
};

struct PacketHeader {
  PacketType type;
  uint32_t sequenceNum;
};

enum class MultiplayerLeaveReason : uint8_t {
  CLIENT_RECONNECTING = 1,
  CLIENT_CLOSED = 2,
  HOST_CLOSED = 3,
};

struct PacketLeave {
  PacketHeader header;
  MultiplayerLeaveReason reason;
};

static_assert(sizeof(PacketLeave) == 6, "PacketLeave must contain a one-byte reason");

struct MPVehicleState {
  uint32_t net_id;
  uint8_t vehicle_type;
  uint8_t color_index;
  uint8_t state_id;
  uint8_t target_aid;
  float x, y, z;
  float quat_x, quat_y, quat_z, quat_w;
  float lin_vel_x, lin_vel_y, lin_vel_z;
  float ang_vel_x, ang_vel_y, ang_vel_z;
  uint8_t state_flags;
  uint8_t pad[3];
  uint32_t rider_aids[4];
};

struct PacketPlayerState {
  PacketHeader header;
  uint32_t netId;
  uint8_t status;
  float x, y, z, angle;
  float vel_x, vel_y, vel_z;
  uint64_t send_tick;
  uint32_t state_id;
  uint32_t level_hash;
  uint32_t darkjak_stage;
  uint64_t clock;
  uint64_t tod_frame;
  float tod_ratio;
  float weather_cloud;
  float weather_fog;
  float weather_rain;
  uint16_t buttons;
  uint8_t leftx, lefty, rightx, righty;
  uint8_t respawn_flags;
  float cam_angle_y;
  uint32_t riding_veh_id;
  uint8_t riding_seat_index;
  uint8_t scene_active;
  uint8_t equipped_weapon;
  uint8_t turret_active;
  // World Sync Fields (Continuous Sync)
  float money;
  float gems;
  float skill;
  uint8_t task_mask[64];
  uint8_t active_task_mask[64];
  MPVehicleState veh_state;
  uint32_t action_seq;
  uint32_t action_state_id;
};
static_assert(sizeof(PacketPlayerState) == 337,
              "PacketPlayerState wire layout must remain explicit");

struct PacketTurretState {
  PacketHeader header;
  uint32_t netId;
  uint32_t turret_aid;
  float roty;
  float rotx;
};

struct PacketGameEvent {
  PacketHeader header;
  uint32_t event_id = 0;
  uint16_t payload_size = 0;
  uint8_t payload[480] = {};
};

#define MAX_ENEMY_SYNC_COUNT 128
#define MAX_ENEMIES_PER_PACKET 30

struct MPEnemyState {
  uint32_t actor_id;
  float x, y, z;
  float quat_x, quat_y, quat_z, quat_w;
  float pad1[3];  // Removed anim_index, anim_frame, last_anim_frame
  int32_t hp;
  uint32_t state;
  uint32_t focus_aid;
  uint8_t attack_flag;
  uint8_t owner;
  uint8_t is_aggro;
  uint8_t pad[5];
  uint64_t last_updated;  // Cross-referenced with C++ enet_time_get()
  uint8_t pad_align[8];   // Pad to 80 bytes (16-byte alignment from GOAL)
};

// Packed structure for network transmission only
struct MPEnemyStatePacked {
  uint32_t actor_id;
  float x, y, z;
  int16_t quat[4];
  int32_t hp;
  uint8_t state;
  uint32_t focus_aid;
  uint8_t flags;   // Bitmask: [0: attack_flag, 1: owner, 2: is_aggro]
  uint8_t pad[1];  // Total size: 32 bytes (4-byte aligned)
};

struct PacketEnemySync {
  PacketHeader header;
  uint32_t count;
  uint64_t timestamp;
  MPEnemyStatePacked enemies[MAX_ENEMIES_PER_PACKET];
};

#define MAX_PEDESTRIAN_SYNC_COUNT 128
#define MAX_PEDESTRIANS_PER_PACKET 35

#define MAX_VEHICLE_SYNC_COUNT 64
#define MAX_VEHICLES_PER_PACKET 20

struct MPPedestrianState {
  uint32_t net_id;
  uint8_t object_type;
  uint8_t object_variance;
  uint8_t state_id;   // Replaces anim_index: numeric pedestrian state ID
  uint8_t pad_align;  // Replaces second pad_align byte
  float x, y, z;
  float quat_x, quat_y, quat_z, quat_w;
  int32_t hp;
  uint8_t flags;
  uint8_t target_aid;  // 0 = none, 1 = Host, 2 = Client
  uint8_t context_align[2];
  uint32_t animation_profile;
  uint32_t vehicle_net_id;
  uint32_t transport_id;
  uint8_t transport_side;
  uint8_t pad[7];
};
static_assert(sizeof(MPPedestrianState) == 64, "MPPedestrianState must be 64 bytes");

struct MPPedestrianStatePacked {
  uint32_t net_id;
  uint8_t object_type;
  uint8_t object_variance;
  float x, y, z;
  int16_t quat[4];
  int32_t hp;
  uint8_t state_id;    // Replaces int16_t anim_index
  uint8_t target_aid;  // Replaces int16_t anim_speed
  uint32_t animation_profile;
  uint32_t vehicle_net_id;
  uint32_t transport_id;
  uint8_t transport_side;
  uint8_t flags;
  uint8_t pad[2];
};
static_assert(sizeof(MPPedestrianStatePacked) == 48, "MPPedestrianStatePacked must be 48 bytes");

struct PacketPedestrianSync {
  PacketHeader header;
  uint32_t count;
  uint64_t timestamp;
  uint32_t level_hash;
  MPPedestrianStatePacked peds[MAX_PEDESTRIANS_PER_PACKET];
};

struct MPVehicleStatePacked {
  uint32_t net_id;
  uint8_t vehicle_type;
  uint8_t color_index;
  uint8_t state_id;
  uint8_t target_aid;
  float x, y, z;
  int16_t quat[4];
  int16_t lin_vel[3];  // Downcast
  int16_t ang_vel[3];  // Downcast
  uint8_t state_flags;
  uint32_t rider_aids[4];
};

struct PacketVehicleSync {
  PacketHeader header;
  uint32_t count;
  uint64_t timestamp;
  uint32_t level_hash;
  MPVehicleStatePacked vehs[MAX_VEHICLES_PER_PACKET];
};

struct MPPalaceSquidState {
  uint32_t active;
  uint32_t state_id;
  int32_t stage;
  int32_t hit_points;
  float shield_hit_points;
  int32_t target_role;
  uint32_t draw_force_fade;
  uint32_t action_seq;
  float x, y, z;
  float quat_x, quat_y, quat_z, quat_w;
  float traj_src_x, traj_src_y, traj_src_z;
  float traj_dest_x, traj_dest_y, traj_dest_z;
  float traj_duration;
  int32_t traj_age;
  uint32_t pad_last_updated;
  uint64_t last_updated;
  uint8_t pad[12];
};
static_assert(sizeof(MPPalaceSquidState) == 116, "MPPalaceSquidState must be 116 bytes");

struct PacketPalaceSquidSync {
  PacketHeader header;
  uint64_t timestamp;
  MPPalaceSquidState state;
};

struct MPWidowState {
  uint32_t active;
  uint32_t state_id;
  float x, y, z;
  uint32_t last_updated;
  float quat_x, quat_y, quat_z, quat_w;
  uint8_t pad[8];
};
static_assert(sizeof(MPWidowState) == 48, "MPWidowState must be 48 bytes");

struct PacketWidowSync {
  PacketHeader header;
  uint64_t timestamp;
  MPWidowState state;
};
static_assert(sizeof(PacketWidowSync) == 61, "PacketWidowSync must be packed");

struct MPAirlockState {
  uint32_t airlock_aid;
  uint32_t state_id;
  uint32_t level_id;
  uint32_t sequence;
  uint32_t last_updated;
  uint8_t pad[12];
};
static_assert(sizeof(MPAirlockState) == 32, "MPAirlockState must be 32 bytes");

#define MAX_AIRLOCK_SYNC_COUNT 4

struct PacketAirlockSync {
  PacketHeader header;
  uint32_t count;
  MPAirlockState states[MAX_AIRLOCK_SYNC_COUNT];
  uint32_t sequence;
};
static_assert(sizeof(PacketAirlockSync) == 141, "PacketAirlockSync must be packed");

struct PacketBootstrap {
  PacketHeader header;
  float money;
  float gems;
  float skill;
  float x, y, z;
  uint32_t host_task;
  uint32_t host_node;
  char host_continue[32];
  uint8_t task_mask[64];
  uint8_t active_task_mask[64];
  uint32_t sync_aids_count;
  uint32_t sync_aids[128];
  uint64_t clock;
  uint64_t tod_frame;
  float tod_ratio;
  float weather_cloud;
  float weather_fog;
  float weather_rain;
  float cam_angle_y;
  uint8_t pad_reserved[16];
};

#pragma pack(pop)
