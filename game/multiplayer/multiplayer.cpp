#include "multiplayer.h"
#include "multiplayer_manager.h"
#include "multiplayer_scanner.h"
#include "multiplayer_types.h"
#include "multiplayer_protocol.h"
#include "game/multiplayer/pedestrian/multiplayer_pedestrian.h"
#include "game/multiplayer/vehicle/multiplayer_vehicle.h"

#include "common/log/log.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/jak2/kscheme.h"
#include "enet/enet.h"
#include <cstring>

namespace {
MultiplayerData gMultiplayerData;
bool g_multi_debug_stop_receive = false;

inline int16_t pack_float_q(float v) {
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  return (int16_t)(v * 32767.0f);
}

inline float unpack_float_q(int16_t v) {
  return (float)v / 32767.0f;
}

size_t counted_packet_size(uint32_t count, size_t element_size) {
  return sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t) + (element_size * count);
}

uint32_t clamp_packet_count(uint32_t count, uint32_t max_count) {
  return (count < max_count) ? count : max_count;
}

bool packet_has_counted_payload(const ENetPacket* packet, uint32_t count, size_t element_size) {
  return packet && packet->dataLength >= counted_packet_size(count, element_size);
}

void reset_remote_traffic_buffers() {
  memset(&gMultiplayerData.traffic_buffer, 0, sizeof(gMultiplayerData.traffic_buffer));
  memset(gMultiplayerData.ped_last_updated, 0, sizeof(gMultiplayerData.ped_last_updated));
  memset(gMultiplayerData.veh_last_updated, 0, sizeof(gMultiplayerData.veh_last_updated));
  gMultiplayerData.last_traffic_sync_time = 0;
}

void handle_packet_receive(LocalPlayerInfoGOAL* local, RemotePlayerInfoGOAL* remote) {
  ENetEvent event;
  uint32_t current_time = enet_time_get();

  while (enet_host_service(gMultiplayerData.host, &event, 0) > 0) {
    if (g_multi_debug_stop_receive) {
      if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        enet_packet_destroy(event.packet);
      }
      continue;
    }

    switch (event.type) {
      case ENET_EVENT_TYPE_RECEIVE:
        gMultiplayerData.last_receive_time = current_time;
        
        if (gMultiplayerData.join_status == (int)MultiplayerStatus::RECONNECTING) {
          lg::info("[Multiplayer] Data packet received. Connection restored. Resuming status {}...", gMultiplayerData.pre_reconnect_status);
          gMultiplayerData.join_status = gMultiplayerData.pre_reconnect_status;
        }

        if (event.packet->dataLength >= sizeof(PacketHeader)) {
          PacketHeader* header = (PacketHeader*)event.packet->data;

          if (header->type == PacketType::STATE_UPDATE &&
              event.packet->dataLength == sizeof(PacketPlayerState)) {
            PacketPlayerState* state = (PacketPlayerState*)event.packet->data;
            auto& entity = gMultiplayerData.remote_entities[state->netId];
            if (state->header.sequenceNum > entity.last_sequence_num) {
              if (state->netId != gMultiplayerData.local_net_id &&
                  state->level_hash != 0 &&
                  gMultiplayerData.last_remote_traffic_level_hash != 0 &&
                  state->level_hash != gMultiplayerData.last_remote_traffic_level_hash) {
                reset_remote_traffic_buffers();
                lg::info("[Multiplayer] Remote level changed. Cleared traffic sync buffers.");
              }
              if (state->netId != gMultiplayerData.local_net_id && state->level_hash != 0) {
                gMultiplayerData.last_remote_traffic_level_hash = state->level_hash;
              }
              entity.status = state->status;
              entity.x = state->x; entity.y = state->y; entity.z = state->z;
              entity.angle = state->angle;
              entity.state_id = state->state_id; // Repurposed as state_id
              entity.level_hash = state->level_hash;
              entity.riding = state->riding;
              entity.clock = state->clock;
              entity.buttons = state->buttons;
              entity.leftx = state->leftx; entity.lefty = state->lefty;
              entity.rightx = state->rightx; entity.righty = state->righty;
              entity.cam_angle_y = state->cam_angle_y;
              entity.last_sequence_num = state->header.sequenceNum;
              memcpy(&entity.veh_state, &state->veh_state, sizeof(MPVehicleState));

              if (state->netId == 0) { // If this is the Host
                remote->money = state->money;
                remote->gems = state->gems;
                remote->skill = state->skill;
                memcpy(remote->task_mask, state->task_mask, 64);
                memcpy(remote->active_task_mask, state->active_task_mask, 64);
              }
            }
          } else if (header->type == PacketType::EVENT_GAME &&
                     event.packet->dataLength == sizeof(PacketGameEvent)) {
            PacketGameEvent* evt = (PacketGameEvent*)event.packet->data;
            uint32_t etype = *(uint32_t*)evt->raw_data;
            lg::info("[Multiplayer] Received Game Event: Type {}", etype);
            gMultiplayerData.inbound_events.push_back(*evt);
          } else if (header->type == PacketType::ENEMY_SYNC &&
                     event.packet->dataLength >= (sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t))) {
            PacketEnemySync* enemy_sync = (PacketEnemySync*)event.packet->data;
            uint32_t enemy_count = clamp_packet_count(enemy_sync->count, MAX_ENEMIES_PER_PACKET);
            if (packet_has_counted_payload(event.packet, enemy_count, sizeof(MPEnemyStatePacked))) {
              gMultiplayerData.last_enemy_sync_time = current_time;
              for (uint32_t i = 0; i < enemy_count; i++) {
                MPEnemyStatePacked* incoming = &enemy_sync->enemies[i];
                if (incoming->actor_id == 0) continue;
                bool found = false;
                for (uint32_t j = 0; j < MAX_ENEMY_SYNC_COUNT; j++) {
                  if (gMultiplayerData.remote_enemy_buffer.remote_enemies[j].actor_id == incoming->actor_id) {
                    auto& state = gMultiplayerData.remote_enemy_buffer.remote_enemies[j];
                    state.actor_id = incoming->actor_id;
                    state.x = incoming->x; state.y = incoming->y; state.z = incoming->z;
                    state.quat_x = unpack_float_q(incoming->quat[0]);
                    state.quat_y = unpack_float_q(incoming->quat[1]);
                    state.quat_z = unpack_float_q(incoming->quat[2]);
                    state.quat_w = unpack_float_q(incoming->quat[3]);
                    state.hp = incoming->hp;
                    state.state = incoming->state;
                    state.focus_aid = incoming->focus_aid;
                    state.attack_flag = (incoming->flags & 1) ? 1 : 0;
                    state.owner = (incoming->flags & 2) ? 1 : 0;
                    state.is_aggro = (incoming->flags & 4) ? 1 : 0;
                    state.last_updated = current_time;
                    found = true; break;
                  }
                }
                if (!found) {
                  for (uint32_t j = 0; j < MAX_ENEMY_SYNC_COUNT; j++) {
                    if (gMultiplayerData.remote_enemy_buffer.remote_enemies[j].actor_id == 0) {
                      auto& state = gMultiplayerData.remote_enemy_buffer.remote_enemies[j];
                      state.actor_id = incoming->actor_id;
                      state.x = incoming->x; state.y = incoming->y; state.z = incoming->z;
                      state.quat_x = unpack_float_q(incoming->quat[0]);
                      state.quat_y = unpack_float_q(incoming->quat[1]);
                      state.quat_z = unpack_float_q(incoming->quat[2]);
                      state.quat_w = unpack_float_q(incoming->quat[3]);
                      state.hp = incoming->hp;
                      state.state = incoming->state;
                      state.focus_aid = incoming->focus_aid;
                      state.attack_flag = (incoming->flags & 1) ? 1 : 0;
                      state.owner = (incoming->flags & 2) ? 1 : 0;
                      state.is_aggro = (incoming->flags & 4) ? 1 : 0;
                      state.last_updated = current_time;
                      found = true; break;
                    }
                  }
                }
              }
              gMultiplayerData.remote_enemy_buffer.remote_count = MAX_ENEMY_SYNC_COUNT;
            }
          } else if (header->type == PacketType::PEDESTRIAN_SYNC &&
                     event.packet->dataLength >= (sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t))) {
            handle_pedestrian_sync_packet(event, gMultiplayerData);
          } else if (header->type == PacketType::VEHICLE_SYNC &&
                     event.packet->dataLength >= (sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t))) {
            handle_vehicle_sync_packet(event, gMultiplayerData);
          } else if (header->type == PacketType::FULL_SYNC &&

                     event.packet->dataLength == sizeof(PacketFullSync)) {
            PacketFullSync* full_sync = (PacketFullSync*)event.packet->data;
            local->sync_money = full_sync->money; local->sync_gems = full_sync->gems; local->sync_skill = full_sync->skill;
            remote->x = full_sync->x; remote->y = full_sync->y; remote->z = full_sync->z;
            local->host_task = full_sync->host_task; local->host_node = full_sync->host_node;
            memcpy(local->host_continue, full_sync->host_continue, 32);
            memcpy(local->task_mask, full_sync->task_mask, 64);
            memcpy(local->active_task_mask, full_sync->active_task_mask, 64);
            local->sync_aids_count = (full_sync->sync_aids_count > 128) ? 128 : full_sync->sync_aids_count;
            local->riding = full_sync->riding;
            memcpy(local->sync_aids, full_sync->sync_aids, sizeof(uint32_t) * 128);
            local->clock = full_sync->clock;
            local->sync_flag = 1;
          }
        }
        enet_packet_destroy(event.packet);
        break;
      case ENET_EVENT_TYPE_CONNECT:
        gMultiplayerData.last_receive_time = current_time;
        if (gMultiplayerData.local_role == 1) {
          lg::info("[Multiplayer] Successfully connected to host.");
          gMultiplayerData.join_status = (int)MultiplayerStatus::CONNECTED_LOBBY;
        } else if (gMultiplayerData.local_role == 0) {
          char ip[64]; enet_address_get_host_ip(&event.peer->address, ip, 64);
          lg::info("[Multiplayer] Client connected from {}:{}", ip, event.peer->address.port);
          if (gMultiplayerData.join_status != (int)MultiplayerStatus::IN_GAME) {
            gMultiplayerData.join_status = (int)MultiplayerStatus::CONNECTED_LOBBY;
          } else {
            gMultiplayerData.pending_full_sync = true;
          }
        }
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        if (gMultiplayerData.local_role == 0) {
          lg::info("[Multiplayer] Client disconnected.");
          gMultiplayerData.remote_entities.erase(1);
        } else {
          lg::warn("[Multiplayer] Host has left the session.");
          gMultiplayerData.join_status = (int)MultiplayerStatus::HOST_LEFT;
        }
        break;
      default:
        break;
    }
  }
}

void handle_packet_send(LocalPlayerInfoGOAL* local, MPEventBufferGOAL* events) {
  PacketPlayerState local_state;
  local_state.header.type = PacketType::STATE_UPDATE;
  local_state.header.sequenceNum = ++gMultiplayerData.sequence_num;
  local_state.netId = gMultiplayerData.local_net_id;
  local_state.status = (uint8_t)gMultiplayerData.join_status;
  local_state.x = local->x; local_state.y = local->y; local_state.z = local->z;
  local_state.angle = local->angle;
  local_state.state_id = (uint32_t)local->state_id;
  local_state.level_hash = local->level;
  local_state.riding = local->riding;
  local_state.clock = local->clock;
  local_state.buttons = local->buttons;
  local_state.leftx = local->leftx; local_state.lefty = local->lefty;
  local_state.rightx = local->rightx; local_state.righty = local->righty;
  local_state.cam_angle_y = local->cam_angle_y;
  local_state.money = local->money;
 local_state.gems = local->gems; local_state.skill = local->skill;
  memcpy(local_state.task_mask, local->task_mask, 64);
  memcpy(local_state.active_task_mask, local->active_task_mask, 64);
  memcpy(&local_state.veh_state, &local->veh_state, sizeof(MPVehicleState));
  MultiplayerManager::broadcast(gMultiplayerData, 0, local_state, ENET_PACKET_FLAG_UNSEQUENCED);

  if (gMultiplayerData.local_role == 0 && gMultiplayerData.pending_full_sync) {
    gMultiplayerData.pending_full_sync = false;
    PacketFullSync sync; memset(&sync, 0, sizeof(PacketFullSync));
    sync.header.type = PacketType::FULL_SYNC;
    sync.header.sequenceNum = ++gMultiplayerData.sequence_num;
    sync.money = local->money; sync.gems = local->gems; sync.skill = local->skill;
    sync.x = local->x; sync.y = local->y; sync.z = local->z;
    sync.host_task = local->host_task; sync.host_node = local->host_node;
    memcpy(sync.host_continue, local->host_continue, 32);
    memcpy(sync.task_mask, local->task_mask, 64);
    memcpy(sync.active_task_mask, local->active_task_mask, 64);
    sync.sync_aids_count = (local->sync_aids_count > 128) ? 128 : local->sync_aids_count;
    sync.riding = local->riding;
    memcpy(sync.sync_aids, local->sync_aids, sizeof(uint32_t) * 128);
    sync.clock = local->clock;
    MultiplayerManager::broadcast(gMultiplayerData, 1, sync, ENET_PACKET_FLAG_RELIABLE);
  }
}

void sync_to_goal(RemotePlayerInfoGOAL* remote_goal) {
  uint32_t other_net_id = (gMultiplayerData.local_role == 0) ? 1 : 0;
  if (gMultiplayerData.remote_entities.count(other_net_id)) {
    auto& remote_state = gMultiplayerData.remote_entities[other_net_id];
    remote_goal->x = remote_state.x; remote_goal->y = remote_state.y; remote_goal->z = remote_state.z;
    remote_goal->angle = remote_state.angle;
    remote_goal->id = other_net_id;
    remote_goal->role = (int32_t)other_net_id;
    remote_goal->state_id = (int32_t)remote_state.state_id; // state_id
    remote_goal->level = remote_state.level_hash;
    remote_goal->status = (remote_state.status > 0) ? (int32_t)remote_state.status : 1;
    remote_goal->packet_id = remote_state.last_sequence_num;
    remote_goal->riding = remote_state.riding;
    remote_goal->clock = remote_state.clock;
    remote_goal->buttons = remote_state.buttons;
    remote_goal->leftx = remote_state.leftx; remote_goal->lefty = remote_state.lefty;
    remote_goal->rightx = remote_state.rightx; remote_goal->righty = remote_state.righty;
    remote_goal->cam_angle_y = remote_state.cam_angle_y;
    memcpy(&remote_goal->veh_state, &remote_state.veh_state, sizeof(MPVehicleState));
  } else { remote_goal->status = 0; }
}
}  // namespace

int pc_multi_get_role() { return gMultiplayerData.local_role; }

void pc_multi_poll(u32 local_ptr, u32 remote_ptr) {
  using namespace jak2;
  try {
    if (!gMultiplayerData.initialized || !gMultiplayerData.host) return;
    static uint32_t last_poll_tick = 0;
    uint32_t current_time = enet_time_get();
    bool is_in_game = (gMultiplayerData.join_status == (int)MultiplayerStatus::IN_GAME);
    bool is_reconnecting = (gMultiplayerData.join_status == (int)MultiplayerStatus::RECONNECTING);
    if (!is_in_game && !is_reconnecting) { if (current_time - last_poll_tick < 100) return; }
    last_poll_tick = current_time;
    LocalPlayerInfoGOAL* local = (LocalPlayerInfoGOAL*)Ptr<u8>(local_ptr).c();
    RemotePlayerInfoGOAL* remote = (RemotePlayerInfoGOAL*)Ptr<u8>(remote_ptr).c();
    if (!local || !remote) return;
    handle_packet_receive(local, remote);
    current_time = enet_time_get();
    for (uint32_t i = 0; i < MAX_ENEMY_SYNC_COUNT; i++) {
      if (gMultiplayerData.remote_enemy_buffer.remote_enemies[i].actor_id != 0 &&
          current_time - gMultiplayerData.remote_enemy_buffer.remote_enemies[i].last_updated > 2000) {
        gMultiplayerData.remote_enemy_buffer.remote_enemies[i].actor_id = 0;
      }
    }
    for (uint32_t i = 0; i < MAX_PEDESTRIAN_SYNC_COUNT; i++) {
      if (gMultiplayerData.traffic_buffer.pedestrians[i].net_id != 0 &&
          current_time - gMultiplayerData.ped_last_updated[i] > 2000) {
        gMultiplayerData.traffic_buffer.pedestrians[i].net_id = 0;
      }
    }
    for (uint32_t i = 0; i < MAX_VEHICLE_SYNC_COUNT; i++) {
      if (gMultiplayerData.traffic_buffer.vehicles[i].net_id != 0 &&
          current_time - gMultiplayerData.veh_last_updated[i] > 2000) {
        gMultiplayerData.traffic_buffer.vehicles[i].net_id = 0;
      }
    }
    bool should_check_timeout = (gMultiplayerData.local_role == 1) || 
                               (gMultiplayerData.local_role == 0 && gMultiplayerData.host->connectedPeers > 0);
    if (should_check_timeout && gMultiplayerData.join_status == (int)MultiplayerStatus::IN_GAME &&
        gMultiplayerData.last_receive_time != 0 && current_time - gMultiplayerData.last_receive_time > 10000) {
      gMultiplayerData.pre_reconnect_status = gMultiplayerData.join_status;
      gMultiplayerData.join_status = (int)MultiplayerStatus::RECONNECTING;
    }
  } catch (...) { lg::error("[Multiplayer] Exception in pc_multi_poll"); }
}

void pc_multi_send_state(u32 local_ptr) {
  if (g_multi_debug_stop_receive) return;
  using namespace jak2;
  try {
    if (!gMultiplayerData.initialized || !gMultiplayerData.host || local_ptr < 0x1000) return;
    LocalPlayerInfoGOAL* local = (LocalPlayerInfoGOAL*)Ptr<u8>(local_ptr).c();
    if (local) handle_packet_send(local, nullptr);
  } catch (...) { lg::error("[Multiplayer] Exception in pc_multi_send_state"); }
}

void pc_multi_receive_state(u32 remote_ptr) {
  using namespace jak2;
  try {
    if (!gMultiplayerData.initialized || !gMultiplayerData.host || remote_ptr < 0x1000) return;
    RemotePlayerInfoGOAL* remote = (RemotePlayerInfoGOAL*)Ptr<u8>(remote_ptr).c();
    if (remote) sync_to_goal(remote);
  } catch (...) { lg::error("[Multiplayer] Exception in pc_multi_receive_state"); }
}

void pc_multi_send_events(u32 event_ptr) {
  if (g_multi_debug_stop_receive) return;
  using namespace jak2;
  try {
    if (!gMultiplayerData.initialized || event_ptr < 0x1000) return;
    MPEventBufferGOAL* events = (MPEventBufferGOAL*)Ptr<u8>(event_ptr).c();
    if (events && events->out_count > 0) {
      for (uint32_t i = 0; i < events->out_count && i < 16; ++i) {
        PacketGameEvent out_event; out_event.header.type = PacketType::EVENT_GAME;
        out_event.header.sequenceNum = ++gMultiplayerData.last_out_event_seq;
        memcpy(out_event.raw_data, &events->out_events[i], sizeof(MPEvent));
        MultiplayerManager::broadcast(gMultiplayerData, gMultiplayerData.local_role, out_event, ENET_PACKET_FLAG_RELIABLE);
      }
      events->out_count = 0;
    }
  } catch (...) { lg::error("[Multiplayer] Exception in pc_multi_send_events"); }
}

void pc_multi_receive_events(u32 event_ptr) {
  using namespace jak2;
  try {
    if (!gMultiplayerData.initialized || event_ptr < 0x1000) return;
    MPEventBufferGOAL* events = (MPEventBufferGOAL*)Ptr<u8>(event_ptr).c();
    if (events) {
      if (!gMultiplayerData.inbound_events.empty()) {
        lg::info("[Multiplayer] Moving {} events to GOAL. Current in_count: {}", 
                 gMultiplayerData.inbound_events.size(), events->in_count);
      }
      while (!gMultiplayerData.inbound_events.empty() && events->in_count < 16) {
        memcpy(&events->in_events[events->in_count++], gMultiplayerData.inbound_events.front().raw_data, sizeof(MPEvent));
        gMultiplayerData.inbound_events.erase(gMultiplayerData.inbound_events.begin());
      }
    }
  } catch (...) { lg::error("[Multiplayer] Exception in pc_multi_receive_events"); }
}

void pc_multi_send_enemies(u32 buffer_ptr) {
  using namespace jak2;
  try {
    if (!gMultiplayerData.initialized || buffer_ptr < 0x1000) return;
    MPEnemySyncBufferGOAL* buffer = (MPEnemySyncBufferGOAL*)Ptr<u8>(buffer_ptr).c();
    if (!buffer || buffer->local_count == 0) return;
    uint32_t total_count = (buffer->local_count < MAX_ENEMY_SYNC_COUNT) ? buffer->local_count : MAX_ENEMY_SYNC_COUNT;
    uint32_t sent_count = 0;
    while (sent_count < total_count) {
      uint32_t chunk_size = (total_count - sent_count < MAX_ENEMIES_PER_PACKET) ? (total_count - sent_count) : MAX_ENEMIES_PER_PACKET;
      PacketEnemySync enemy_packet; enemy_packet.header.type = PacketType::ENEMY_SYNC;
      enemy_packet.header.sequenceNum = ++gMultiplayerData.sequence_num;
      enemy_packet.count = chunk_size; enemy_packet.timestamp = enet_time_get();
      for (uint32_t i = 0; i < chunk_size; i++) {
        auto* src = &buffer->local_enemies[sent_count + i]; auto* dst = &enemy_packet.enemies[i];
        dst->actor_id = src->actor_id; dst->x = src->x; dst->y = src->y; dst->z = src->z;
        dst->quat[0] = pack_float_q(src->quat_x); dst->quat[1] = pack_float_q(src->quat_y);
        dst->quat[2] = pack_float_q(src->quat_z); dst->quat[3] = pack_float_q(src->quat_w);
        dst->hp = src->hp; dst->state = src->state; dst->focus_aid = src->focus_aid;
        dst->flags = (src->attack_flag ? 1 : 0) | (src->owner ? 2 : 0) | (src->is_aggro ? 4 : 0);
      }
      size_t packet_size = sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t) + (sizeof(MPEnemyStatePacked) * chunk_size);
      MultiplayerManager::broadcast(gMultiplayerData, gMultiplayerData.local_role, &enemy_packet, packet_size, ENET_PACKET_FLAG_UNSEQUENCED);
      sent_count += chunk_size;
    }
  } catch (...) { lg::error("[Multiplayer] Exception in pc_multi_send_enemies"); }
}

void pc_multi_receive_enemies(u32 buffer_ptr) {
  using namespace jak2;
  try {
    if (!gMultiplayerData.initialized || buffer_ptr < 0x1000) return;
    MPEnemySyncBufferGOAL* buffer = (MPEnemySyncBufferGOAL*)Ptr<u8>(buffer_ptr).c();
    if (buffer) {
      buffer->remote_count = gMultiplayerData.remote_enemy_buffer.remote_count;
      memcpy(buffer->remote_enemies, gMultiplayerData.remote_enemy_buffer.remote_enemies, sizeof(MPEnemyState) * MAX_ENEMY_SYNC_COUNT);
      buffer->last_sync_time = gMultiplayerData.last_enemy_sync_time;
    }
  } catch (...) { lg::error("[Multiplayer] Exception in pc_multi_receive_enemies"); }
}

void pc_multi_send_traffic(u32 buffer_ptr) {
  using namespace jak2;
  try {
    if (!gMultiplayerData.initialized || gMultiplayerData.local_role != 0 || buffer_ptr < 0x1000) return;
    MPTrafficSyncBufferGOAL* buffer = (MPTrafficSyncBufferGOAL*)Ptr<u8>(buffer_ptr).c();
    if (!buffer) return;
    int exclude_peer = (int)gMultiplayerData.local_role;

    send_pedestrian_sync_packets(gMultiplayerData, buffer, exclude_peer);
    send_vehicle_sync_packets(gMultiplayerData, buffer, exclude_peer);

  } catch (...) { lg::error("[Multiplayer] Exception in pc_multi_send_traffic"); }
}

void pc_multi_receive_traffic(u32 buffer_ptr) {
  using namespace jak2;
  try {
    if (!gMultiplayerData.initialized || buffer_ptr < 0x1000) return;
    MPTrafficSyncBufferGOAL* buffer = (MPTrafficSyncBufferGOAL*)Ptr<u8>(buffer_ptr).c();
    if (buffer) {
      receive_pedestrian_sync_data(gMultiplayerData, buffer);
      receive_vehicle_sync_data(gMultiplayerData, buffer);
    }
  } catch (...) { lg::error("[Multiplayer] Exception in pc_multi_receive_traffic"); }
}

u64 pc_multi_get_enemy_sync_time() { return gMultiplayerData.last_enemy_sync_time; }
void pc_multi_disconnect() { MultiplayerManager::disconnect(gMultiplayerData); }
void pc_multi_setup_host() { MultiplayerManager::setup_host(gMultiplayerData); }
void pc_multi_setup_client(u32 ip_ptr, u32 port) { using namespace jak2; MultiplayerManager::setup_client(gMultiplayerData, Ptr<String>(ip_ptr).c()->data(), (int)port); }
int pc_multi_get_status() { return MultiplayerScanner::get_status(gMultiplayerData); }
void pc_multi_set_status(int status) {
  int old_status = gMultiplayerData.join_status; gMultiplayerData.join_status = status;
  if (old_status != status) lg::info("[Multiplayer] Status transition: {} -> {}", old_status, status);
  if (gMultiplayerData.local_role == 0 && status == 6 && old_status != 6) gMultiplayerData.pending_full_sync = true;
}
void pc_multi_stop_search() { MultiplayerScanner::stop_search(gMultiplayerData); }
void pc_multi_start_search() { MultiplayerScanner::start_search(gMultiplayerData); }
u64 pc_multi_get_command_line_arg(u32 str_ptr) {
  using namespace jak2; const char* arg_name = Ptr<String>(str_ptr).c()->data();
  for (int i = 1; i < g_argc; i++) { if (strcmp(g_argv[i], arg_name) == 0) return make_string_from_c(i + 1 < g_argc ? g_argv[i+1] : ""); }
  return s7.offset;
}
u64 pc_multi_get_found_ip() { using namespace jak2; return make_string_from_c(gMultiplayerData.found_ip.c_str()); }
void pc_multi_debug_stop_receive(u32 val) { g_multi_debug_stop_receive = (val != 0); }
u64 pc_multi_get_ticks() { return enet_time_get(); }

int pc_multi_get_ping() {
  if (!gMultiplayerData.host)
    return 0;
  if (gMultiplayerData.server_peer) {
    return gMultiplayerData.server_peer->roundTripTime;
  }

  u32 total = 0;
  u32 count = 0;
  for (size_t i = 0; i < gMultiplayerData.host->peerCount; i++) {
    if (gMultiplayerData.host->peers[i].state == ENET_PEER_STATE_CONNECTED) {
      total += gMultiplayerData.host->peers[i].roundTripTime;
      count++;
    }
  }
  return count > 0 ? (total / count) : 0;
}

int pc_multi_get_packet_loss() {
  if (!gMultiplayerData.host)
    return 0;
  if (gMultiplayerData.server_peer) {
    return (gMultiplayerData.server_peer->packetLoss * 100) / 65536;
  }

  u32 total = 0;
  u32 count = 0;
  for (size_t i = 0; i < gMultiplayerData.host->peerCount; i++) {
    if (gMultiplayerData.host->peers[i].state == ENET_PEER_STATE_CONNECTED) {
      total += (gMultiplayerData.host->peers[i].packetLoss * 100) / 65536;
      count++;
    }
  }
  return count > 0 ? (total / count) : 0;
}

void init_multiplayer_pc_port() {
  using namespace jak2;
  make_function_symbol_from_c("pc-multi-setup-host", (void*)pc_multi_setup_host);
  make_function_symbol_from_c("pc-multi-setup-client", (void*)pc_multi_setup_client);
  make_function_symbol_from_c("pc-multi-get-status", (void*)pc_multi_get_status);
  make_function_symbol_from_c("pc-multi-set-status", (void*)pc_multi_set_status);
  make_function_symbol_from_c("pc-multi-stop-search", (void*)pc_multi_stop_search);
  make_function_symbol_from_c("pc-multi-start-search", (void*)pc_multi_start_search);
  make_function_symbol_from_c("pc-multi-get-found-ip", (void*)pc_multi_get_found_ip);
  make_function_symbol_from_c("pc-multi-poll", (void*)pc_multi_poll);
  make_function_symbol_from_c("pc-multi-send-state", (void*)pc_multi_send_state);
  make_function_symbol_from_c("pc-multi-receive-state", (void*)pc_multi_receive_state);
  make_function_symbol_from_c("pc-multi-send-events", (void*)pc_multi_send_events);
  make_function_symbol_from_c("pc-multi-receive-events", (void*)pc_multi_receive_events);
  make_function_symbol_from_c("pc-multi-send-enemies", (void*)pc_multi_send_enemies);
  make_function_symbol_from_c("pc-multi-receive-enemies", (void*)pc_multi_receive_enemies);
  make_function_symbol_from_c("pc-multi-send-traffic", (void*)pc_multi_send_traffic);
  make_function_symbol_from_c("pc-multi-receive-traffic", (void*)pc_multi_receive_traffic);
  make_function_symbol_from_c("pc-multi-get-enemy-sync-time", (void*)pc_multi_get_enemy_sync_time);
  make_function_symbol_from_c("pc-multi-get-role", (void*)pc_multi_get_role);
  make_function_symbol_from_c("pc-multi-disconnect", (void*)pc_multi_disconnect);
  make_function_symbol_from_c("pc-multi-get-command-line-arg", (void*)pc_multi_get_command_line_arg);
  make_function_symbol_from_c("pc-multi-debug-stop-receive", (void*)pc_multi_debug_stop_receive);
  make_function_symbol_from_c("pc-multi-get-ticks", (void*)pc_multi_get_ticks);
  make_function_symbol_from_c("pc-multi-get-ping", (void*)pc_multi_get_ping);
  make_function_symbol_from_c("pc-multi-get-packet-loss", (void*)pc_multi_get_packet_loss);
}
