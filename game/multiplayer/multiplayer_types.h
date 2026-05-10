#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <string>
#include <thread>

#include "multiplayer_protocol.h"

struct RemoteEntityState {
  uint8_t status;
  float x, y, z, angle;
  uint32_t state_id;
  uint32_t level_hash;
  uint32_t riding;
  uint64_t clock;
  uint16_t buttons;
  uint8_t leftx, lefty, rightx, righty;
  float cam_angle_y;
  uint32_t riding_veh_id;
  uint8_t riding_seat_index;
  uint32_t last_sequence_num = 0;
  MPVehicleState veh_state;
};

struct MPEvent {
  uint32_t etype;
  uint32_t pad;
  uint8_t data[488];
};

struct MPEventBufferGOAL {
  uint32_t out_count;
  uint8_t pad1[12];
  MPEvent out_events[16];
  uint32_t in_count;
  uint8_t pad2[12];
  MPEvent in_events[16];
};

struct RemotePlayerInfoGOAL {
  float x, y, z, angle;
  uint32_t id;
  int32_t role;
  uint32_t state_id;
  uint32_t level;
  int32_t status;
  uint32_t packet_id;
  uint32_t riding;
  uint32_t pad_clock;
  uint64_t clock;
  uint16_t buttons;
  uint8_t leftx, lefty, rightx, righty;
  uint16_t pad_cam;
  float cam_angle_y;
  uint32_t riding_veh_id;
  uint8_t riding_seat_index;
  uint8_t pad_reserved[3];
  // World Sync Fields (Mirrored from local-player-info)
  float money;
  float gems;
  float skill;
  float sync_money;
  float sync_gems;
  float sync_skill;
  uint32_t sync_flag;
  uint32_t host_task;
  uint32_t host_node;
  uint8_t host_continue[32];
  uint8_t task_mask[64];
  uint8_t active_task_mask[64];
  uint32_t sync_aids_count;
  uint32_t sync_aids[128];
  uint64_t player_procs[2];
  MPVehicleState veh_state;
};

struct LocalPlayerInfoGOAL {
  float x, y, z, angle;
  uint32_t id; // Placeholder
  int32_t role;
  uint32_t state_id;
  uint32_t level;
  int32_t status; // Placeholder
  uint32_t packet_id;
  uint32_t riding;
  uint32_t pad_clock;
  uint64_t clock;
  uint16_t buttons;
  uint8_t leftx, lefty, rightx, righty;
  uint16_t pad_cam;
  float cam_angle_y;
  uint32_t riding_veh_id;
  uint8_t riding_seat_index;
  uint8_t pad_reserved[3];
  // Global World Sync (Outgoing)
  float money;
  float gems;
  float skill;
  // Global World Sync (Incoming)
  float sync_money;
  float sync_gems;
  float sync_skill;
  uint32_t sync_flag;
  uint32_t host_task;
  uint32_t host_node;
  uint8_t host_continue[32];
  uint8_t task_mask[64];
  uint8_t active_task_mask[64];
  uint32_t sync_aids_count;
  uint32_t sync_aids[128];
  uint64_t player_procs[2];
  MPVehicleState veh_state;
};

struct MPEnemySyncBufferGOAL {
  uint32_t local_count;
  uint8_t pad1[12];
  MPEnemyState local_enemies[MAX_ENEMY_SYNC_COUNT];
  uint32_t remote_count;
  uint8_t pad2[12];
  MPEnemyState remote_enemies[MAX_ENEMY_SYNC_COUNT];
  uint64_t last_sync_time;
};

struct MPTrafficSyncBufferGOAL {
  uint32_t ped_count;
  uint8_t pad1[12];
  MPPedestrianState pedestrians[MAX_PEDESTRIAN_SYNC_COUNT];
  uint32_t veh_count;
  uint8_t pad2[12];
  MPVehicleState vehicles[MAX_VEHICLE_SYNC_COUNT];
  uint64_t last_sync_time;
};

struct MultiplayerData {
  bool initialized = false;
  bool enet_initialized = false;
  struct _ENetHost* host = nullptr;
  struct _ENetPeer* server_peer = nullptr;  // Only used if we are a client
  int local_role = -1;
  uint32_t local_net_id = 0;
  uint32_t sequence_num = 0;
  uint32_t last_out_event_seq = 0;

  std::unordered_map<uint32_t, RemoteEntityState> remote_entities;
  std::vector<PacketGameEvent> inbound_events;
  MPEnemySyncBufferGOAL remote_enemy_buffer;
  uint32_t last_enemy_sync_time = 0;

  MPTrafficSyncBufferGOAL traffic_buffer;
  uint32_t last_traffic_sync_time = 0;
  uint32_t last_remote_traffic_level_hash = 0;
  uint64_t ped_last_updated[MAX_PEDESTRIAN_SYNC_COUNT] = {0};
  uint64_t veh_last_updated[MAX_VEHICLE_SYNC_COUNT] = {0};

  // New fields for joining/searching
  std::atomic<int> join_status{0}; // 0: idle, 1: searching, 2: found, 3: connecting, 4: connected, -1: failed
  std::string found_ip = "";
  std::atomic<bool> stop_search{false};

  // Discovery / Hosting responder
  std::thread discovery_thread;
  std::atomic<bool> host_discovery_active{false};
  std::atomic<bool> pending_full_sync{false};

  // Reconnection tracking
  uint32_t last_receive_time = 0;
  int pre_reconnect_status = 0;
};
