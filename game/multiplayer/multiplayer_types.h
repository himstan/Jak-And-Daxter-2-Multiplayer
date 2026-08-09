#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "multiplayer_port_mapping.h"
#include "multiplayer_packet_scheduler.h"
#include "multiplayer_protocol.h"
#include "multiplayer_ring_buffer.h"
#include "multiplayer_security.h"
#include "multiplayer_stats.h"

struct CachedPlayerState {
  bool identity_ready = false;
  bool state_ready = false;
  uint32_t player_id = kMPInvalidPlayerId;
  MPPlayerCharacter character = MPPlayerCharacter::UNKNOWN;
  char player_name[kMultiplayerPlayerNameSize] = {};
  uint8_t status;
  float x, y, z, angle;
  float vel_x, vel_y, vel_z;
  uint64_t send_tick;
  uint64_t receive_tick;
  uint32_t state_id;
  uint32_t level_hash;
  uint32_t darkjak_stage;
  uint16_t buttons;
  uint8_t leftx, lefty, rightx, righty;
  uint8_t respawn_flags;
  float cam_angle_y;
  uint32_t riding_veh_id;
  uint8_t riding_seat_index;
  uint8_t scene_active;
  uint8_t equipped_weapon;
  uint8_t turret_active;
  uint32_t action_seq;
  uint32_t action_state_id;
  float turret_roty;
  float turret_rotx;
  uint32_t last_sequence_num = 0;
  uint32_t last_turret_sequence_num = 0;
  MPVehicleState veh_state;
};

struct MPEvent {
  uint32_t etype;
  uint32_t payload_size;
  uint32_t source_player_id;
  uint8_t pad[4];
  uint8_t data[480];
};
static_assert(sizeof(MPEvent) == 496, "MPEvent must match GOAL mp-event");

struct MPEventBufferGOAL {
  uint32_t out_count;
  uint8_t pad1[12];
  MPEvent out_events[16];
  uint32_t in_count;
  uint8_t pad2[12];
  MPEvent in_events[16];
};
static_assert(sizeof(MPEventBufferGOAL) == 15904, "MPEventBufferGOAL must match GOAL");

struct MPPlayerIdentityGOAL {
  uint32_t player_id;
  MPPlayerCharacter character;
  uint8_t identity_ready;
  uint8_t state_ready;
  uint8_t joined;
  uint8_t reserved;
  char name[kMultiplayerPlayerNameSize];
  uint8_t pad[12];
};
static_assert(sizeof(MPPlayerIdentityGOAL) == 48);

struct MPPlayerConnectionStateGOAL {
  int32_t status;
  uint32_t latest_state_sequence;
  uint32_t latest_turret_sequence;
  uint8_t connected;
  uint8_t state_cached;
  uint8_t pad[2];
};
static_assert(sizeof(MPPlayerConnectionStateGOAL) == 16);

struct MPPlayerTransformStateGOAL {
  float position[4];
  float velocity[4];
  float angle;
  uint32_t level;
  uint32_t reserved[2];
};
static_assert(sizeof(MPPlayerTransformStateGOAL) == 48);

struct MPPlayerActionStateGOAL {
  uint32_t target_state_id;
  uint32_t darkjak_stage;
  uint32_t authoritative_sequence;
  uint32_t action_state_id;
  uint8_t scene_state;
  uint8_t respawn_flags;
  uint8_t death_state;
  uint8_t scene_latched;
  uint32_t last_replayed_sequence;
  uint32_t reserved[2];
};
static_assert(sizeof(MPPlayerActionStateGOAL) == 32);

struct MPPlayerInputStateGOAL {
  uint16_t buttons;
  uint8_t leftx, lefty, rightx, righty;
  uint8_t equipped_weapon;
  uint8_t reserved;
  float camera_angle_y;
  uint32_t pad;
};
static_assert(sizeof(MPPlayerInputStateGOAL) == 16);

struct MPPlayerVehicleStateGOAL {
  uint32_t vehicle_id;
  uint8_t seat_index;
  uint8_t turret_active;
  uint8_t reserved[2];
  float turret_roty;
  float turret_rotx;
  MPVehicleState state;
};
static_assert(sizeof(MPPlayerVehicleStateGOAL) == 96);

struct MPTargetGhostRecordGOAL {
  float trans[4];
  float quat[4];
  float velocity[4];
  uint32_t state_id;
  uint8_t equipped_weapon;
  uint8_t pad_reserved[3];
  uint16_t buttons;
  uint8_t leftx, lefty, rightx, righty;
  uint8_t pad_before_camera[2];
  float camera_angle_y;
  float health;
  uint64_t last_update;
  uint32_t active;
  uint8_t pad[12];
};
static_assert(sizeof(MPTargetGhostRecordGOAL) == 96);

struct MPPlayerRuntimeStateGOAL {
  uint64_t target_handle;
  uint64_t vehicle_handle;
  MPTargetGhostRecordGOAL ghost;
  float presentation_position[4];
  float presentation_quaternion[4];
  uint64_t last_fresh_input_time;
  uint64_t action_warmup_start;
  uint32_t last_state_packet_id;
  uint32_t last_action_sequence;
  int32_t state_mismatch_count;
  uint32_t death_state;
  uint32_t pending_gun_shot_sequence;
  uint32_t last_gun_shot_sequence;
  uint32_t last_gun_replay_debug_sequence;
  float interpolation_angle;
  float pending_gun_shot_camera_angle;
  int32_t last_gun_log_active;
  int32_t last_gun_log_requested;
  uint8_t pad_index;
  uint8_t puppet_lifecycle;
  uint8_t pending_gun_shot_weapon;
  uint8_t reserved_byte;
  uint32_t flags;
  uint32_t reserved[3];
};
static_assert(sizeof(MPPlayerRuntimeStateGOAL) == 224);

struct MPPlayerRecordGOAL {
  MPPlayerIdentityGOAL identity;
  MPPlayerConnectionStateGOAL connection;
  MPPlayerTransformStateGOAL transform;
  MPPlayerActionStateGOAL action;
  MPPlayerInputStateGOAL input;
  MPPlayerVehicleStateGOAL vehicle;
  MPPlayerRuntimeStateGOAL runtime;
};
static_assert(sizeof(MPPlayerRecordGOAL) == 480);

struct MPPlayerControllerGOAL {
  MPPlayerRecordGOAL records[kMPMaxPlayers];
  uint32_t local_player_id;
  uint32_t host_player_id;
  uint32_t reserved[2];
};
static_assert(sizeof(MPPlayerControllerGOAL) == 1936);

struct MPWorldSyncStateGOAL {
  float money;
  float gems;
  float skill;
  uint32_t sequence;
  uint64_t clock;
  uint64_t time_of_day_frame;
  float time_of_day_ratio;
  float weather_cloud;
  float weather_fog;
  float weather_rain;
  uint8_t task_mask[64];
  uint8_t active_task_mask[64];
};
static_assert(sizeof(MPWorldSyncStateGOAL) == 176);

struct MPBootstrapSyncStateGOAL {
  uint32_t phase;
  uint32_t sequence;
  uint32_t host_task;
  uint32_t reserved;
  uint8_t host_continue[32];
  float host_spawn_position[4];
  float host_spawn_angle;
  float host_camera_angle_y;
  uint32_t synchronized_aid_count;
  uint32_t pad_before_aids;
  uint32_t synchronized_aids[128];
};
static_assert(sizeof(MPBootstrapSyncStateGOAL) == 592);

struct MPEnemySyncBufferGOAL {
  uint32_t local_count;
  uint8_t pad1[12];
  MPEnemyState local_enemies[MAX_ENEMY_SYNC_COUNT];
  uint32_t remote_count;
  uint8_t pad2[12];
  MPEnemyState remote_enemies[MAX_ENEMY_SYNC_COUNT];
  uint64_t last_sync_time;
};
static_assert(sizeof(MPEnemySyncBufferGOAL) == 20520, "MPEnemySyncBufferGOAL must match GOAL");

struct MPTrafficSyncBufferGOAL {
  uint32_t ped_count;
  uint8_t pad1[12];
  MPPedestrianState pedestrians[MAX_PEDESTRIAN_SYNC_COUNT];
  uint32_t veh_count;
  uint8_t pad2[12];
  MPVehicleState vehicles[MAX_VEHICLE_SYNC_COUNT];
  uint64_t last_sync_time;
};
static_assert(sizeof(MPTrafficSyncBufferGOAL) == 13352, "MPTrafficSyncBufferGOAL must match GOAL");

struct MPPalaceSquidSyncBufferGOAL {
  MPPalaceSquidState local_state;
  uint8_t pad_after_local[12];
  MPPalaceSquidState remote_state;
  uint8_t pad_before_last_sync[4];
  uint64_t last_sync_time;
  uint8_t pad[8];
};
static_assert(sizeof(MPPalaceSquidSyncBufferGOAL) == 264,
              "MPPalaceSquidSyncBufferGOAL must match GOAL");

struct MPWidowSyncBufferGOAL {
  MPWidowState local_state;
  MPWidowState remote_state;
  uint64_t last_sync_time;
  uint8_t pad[8];
};
static_assert(sizeof(MPWidowSyncBufferGOAL) == 112,
              "MPWidowSyncBufferGOAL must match GOAL");

struct MPAirlockStateGOAL {
  uint32_t airlock_aid;
  uint32_t state_id;
  uint32_t level_id;
  uint32_t sequence;
  uint32_t last_updated;
  uint8_t pad[12];
};
static_assert(sizeof(MPAirlockStateGOAL) == 32, "MPAirlockStateGOAL must match GOAL");

struct MPAirlockStateTableGOAL {
  uint32_t count;
  uint8_t pad[12];
  MPAirlockStateGOAL states[MAX_AIRLOCK_SYNC_COUNT];
};
static_assert(sizeof(MPAirlockStateTableGOAL) == 144, "MPAirlockStateTableGOAL must match GOAL");

struct MPAirlockSyncBufferGOAL {
  MPAirlockStateTableGOAL local_table;
  MPAirlockStateTableGOAL remote_table;
  uint32_t sequence;
  uint8_t pad_before_last_sync[4];
  uint64_t last_sync_time;
  uint8_t pad[8];
};
static_assert(sizeof(MPAirlockSyncBufferGOAL) == 312, "MPAirlockSyncBufferGOAL must match GOAL");

struct MultiplayerData {
  struct AuthenticationFailure {
    uint32_t address = 0;
    uint32_t window_start = 0;
    uint32_t banned_until = 0;
    uint8_t count = 0;
  };

  struct PendingHandshake {
    struct _ENetPeer* peer = nullptr;
    uint32_t deadline = 0;
  };

  bool initialized = false;
  bool enet_initialized = false;
  struct _ENetHost* host = nullptr;
  struct _ENetPeer* server_peer = nullptr;         // Only used if we are a client
  struct _ENetPeer* authenticated_peer = nullptr;  // Only used if we are a host
  int session_role = -1;
  uint32_t local_player_id = kMPInvalidPlayerId;
  uint32_t host_player_id = kMPInvalidPlayerId;
  uint32_t authenticated_player_id = kMPInvalidPlayerId;
  uint32_t sequence_num = 0;
  uint32_t last_out_event_seq = 0;
  MultiplayerSecurity security;
  bool internet_host = false;
  bool host_game_active = false;
  uint32_t handshake_started_time = 0;
  std::array<AuthenticationFailure, 16> authentication_failures = {};
  size_t next_authentication_failure_slot = 0;
  std::array<PendingHandshake, 8> pending_handshakes = {};
  std::string staged_invite;
  int staged_invite_status = 0;
  std::string reconnect_invite;
  std::array<char, 16> direct_address = {};
  std::array<char, 6> direct_port = {};
  std::array<char, 7> direct_room_code = {};
  std::string local_version;
  std::string required_version;
  std::atomic<int> connection_phase{static_cast<int>(MultiplayerConnectionPhase::IDLE)};
  std::atomic<int> connection_failure{static_cast<int>(MultiplayerConnectionFailure::NONE)};

  std::array<CachedPlayerState, kMPMaxPlayers> player_states = {};
  uint32_t last_world_sequence = 0;
  MultiplayerRingBuffer<PacketGameEvent, 64> inbound_events;
  MPEnemySyncBufferGOAL remote_enemy_buffer;
  uint32_t last_enemy_sync_time = 0;
  uint32_t last_enemy_sequence = 0;

  MPTrafficSyncBufferGOAL traffic_buffer;
  uint32_t last_traffic_sync_time = 0;
  uint32_t last_pedestrian_sequence = 0;
  uint32_t local_traffic_level_hash = 0;
  uint32_t last_remote_traffic_level_hash = 0;
  uint32_t remote_traffic_buffer_level_hash = 0;
  uint32_t last_ped_traffic_debug_time = 0;
  uint32_t last_veh_traffic_debug_time = 0;
  uint32_t last_traffic_drop_debug_time = 0;
  uint32_t last_traffic_short_packet_debug_time = 0;
  uint64_t ped_last_updated[MAX_PEDESTRIAN_SYNC_COUNT] = {0};
  uint64_t veh_last_updated[MAX_VEHICLE_SYNC_COUNT] = {0};
  uint32_t veh_last_sequence[MAX_VEHICLE_SYNC_COUNT] = {0};

  MPPalaceSquidState remote_palace_squid_state = {};
  uint32_t last_palace_squid_sync_time = 0;

  MPWidowState remote_widow_state = {};
  uint32_t last_widow_sync_time = 0;
  uint32_t last_widow_sequence = 0;

  MPAirlockStateTableGOAL remote_airlock_table = {};
  uint32_t last_airlock_sync_time = 0;
  uint32_t last_remote_airlock_sequence = 0;

  // New fields for joining/searching
  std::atomic<int> join_status{
      0};  // 0: idle, 1: searching, 2: found, 3: connecting, 4: connected, -1: failed
  std::string found_ip = "";
  std::mutex discovery_result_mutex;
  std::atomic<bool> stop_search{false};
  bool directed_discovery = false;
  uint32_t directed_discovery_address = 0;
  uint16_t directed_discovery_game_port = 0;

  // Discovery / Hosting responder
  std::thread discovery_thread;
  std::thread scanner_thread;
  std::atomic<bool> host_discovery_active{false};
  std::atomic<bool> pending_bootstrap{false};
  bool join_identity_sent = false;
  bool pending_bootstrap_sent_once = false;
  uint32_t last_bootstrap_send_time = 0;
  uint32_t last_event_queue_debug_time = 0;
  uint32_t last_event_receive_debug_time = 0;

  // Reconnection tracking
  uint32_t last_authenticated_receive_time = 0;
  bool reconnect_attempt_active = false;
  bool reconnect_waiting_for_bootstrap = false;
  uint32_t reconnect_attempt_count = 0;
  uint32_t reconnect_next_attempt_time = 0;

  // Host-side temporary router mapping. The address remains private to the bridge.
  std::thread port_mapping_thread;
  std::mutex port_mapping_mutex;
  std::condition_variable port_mapping_cv;
  std::atomic<bool> port_mapping_worker_stop{false};
  MPPortMappingState port_mapping_state = MPPortMappingState::IDLE;
  MPPortMappingMethod port_mapping_method = MPPortMappingMethod::NONE;
  uint16_t port_mapping_local_port = 0;
  uint16_t port_mapping_external_port = 0;
  std::string port_mapping_external_ip;
  std::atomic<int> host_setup_status{static_cast<int>(MultiplayerHostSetupStatus::IDLE)};
  // Rate tracking and statistics
  MultiplayerPacketScheduler packet_scheduler;
  MultiplayerStats stats;
};
