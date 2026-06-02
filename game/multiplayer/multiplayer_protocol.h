#pragma once

#include <cstdint>

#pragma pack(push, 1)

const int DISCOVERY_PORT = 3001;
const char* const DISCOVERY_MAGIC = "OG_MP_DISCOVERY";

enum class MultiplayerStatus : uint8_t {
  IDLE = 0,
  SEARCHING = 1,
  FOUND = 2,
  CONNECTING = 3,
  CONNECTED_LOBBY = 4, // Both connected, waiting in menu
  GAME_STARTING = 5,   // Host/Client is loading the save/level
  IN_GAME = 6,         // Fully spawned and playing
  RECONNECTING = 7,    // Connection lost, trying to restore
  HOST_LEFT = 8,       // Host has explicitly left the session
  FAILED = 255
};

enum class PacketType : uint8_t {
  STATE_UPDATE = 0,
  EVENT_JOIN = 1,
  EVENT_LEAVE = 2,
  EVENT_GAME = 3,
  FULL_SYNC = 4,
  ENEMY_SYNC = 5,
  PEDESTRIAN_SYNC = 6,
  VEHICLE_SYNC = 7,
  TURRET_SYNC = 8,
  PALACE_SQUID_SYNC = 9
};

struct PacketHeader {
  PacketType type;
  uint32_t sequenceNum;
};

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
  uint32_t riding;
  uint64_t clock;
  uint64_t tod_frame;
  float tod_ratio;
  float weather_cloud;
  float weather_fog;
  float weather_rain;
  uint16_t buttons;
  uint8_t leftx, lefty, rightx, righty;
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
};

struct PacketTurretState {
  PacketHeader header;
  uint32_t netId;
  uint32_t turret_aid;
  float roty;
  float rotx;
};

struct PacketGameEvent {
  PacketHeader header;
  uint8_t pad[3]; // Align to 8 bytes for GOAL mapping
  uint8_t raw_data[496];
};

struct WorldEventData {
  uint32_t actor_id;
  uint32_t attack_id;
};

struct SceneEventData {
  uint32_t state;
  char scene_name[32];
  uint32_t event_id;
};

#define MAX_ENEMY_SYNC_COUNT 128
#define MAX_ENEMIES_PER_PACKET 30

struct MPEnemyState {
  uint32_t actor_id;
  float x, y, z;
  float quat_x, quat_y, quat_z, quat_w;
  float pad1[3]; // Removed anim_index, anim_frame, last_anim_frame
  int32_t hp;
  uint32_t state;
  uint32_t focus_aid;
  uint8_t attack_flag;
  uint8_t owner;
  uint8_t is_aggro;
  uint8_t pad[5];
  uint64_t last_updated; // Cross-referenced with C++ enet_time_get()
  uint8_t pad_align[8];  // Pad to 80 bytes (16-byte alignment from GOAL)
};

// Packed structure for network transmission only
struct MPEnemyStatePacked {
  uint32_t actor_id;
  float x, y, z;
  int16_t quat[4];
  int32_t hp;
  uint8_t state;
  uint32_t focus_aid;
  uint8_t flags; // Bitmask: [0: attack_flag, 1: owner, 2: is_aggro]
  uint8_t pad[1]; // Total size: 32 bytes (4-byte aligned)
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
  uint8_t target_aid; // 0 = none, 1 = Host, 2 = Client
  uint8_t pad[22];    // Reduced from 26 to compensate for target-aid and hp
};
static_assert(sizeof(MPPedestrianState) == 64, "MPPedestrianState must be 64 bytes");

struct MPPedestrianStatePacked {
  uint32_t net_id;
  uint8_t object_type;
  uint8_t object_variance;
  float x, y, z;
  int16_t quat[4];
  int32_t hp;
  uint8_t state_id;   // Replaces int16_t anim_index
  uint8_t target_aid; // Replaces int16_t anim_speed
  uint32_t vehicle_net_id;
  uint8_t pad[2];
};


struct PacketPedestrianSync {
  PacketHeader header;
  uint32_t count;
  uint64_t timestamp;
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
  int16_t lin_vel[3]; // Downcast
  int16_t ang_vel[3]; // Downcast
  uint8_t state_flags;
  uint32_t rider_aids[4];
};

struct PacketVehicleSync {
  PacketHeader header;
  uint32_t count;
  uint64_t timestamp;
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

struct PacketFullSync {
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
  uint32_t riding;
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
