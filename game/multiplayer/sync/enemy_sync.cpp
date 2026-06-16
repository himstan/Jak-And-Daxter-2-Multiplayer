#include "enemy_sync.h"

#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"

#include <cstring>

namespace {
size_t enemy_packet_size(uint32_t count) {
  return mp_counted_packet_size(count, sizeof(MPEnemyStatePacked));
}

MPEnemyState* find_enemy_slot(MPEnemySyncBufferGOAL& buffer, uint32_t actor_id) {
  MPEnemyState* empty_slot = nullptr;
  for (uint32_t i = 0; i < MAX_ENEMY_SYNC_COUNT; ++i) {
    auto& state = buffer.remote_enemies[i];
    if (state.actor_id == actor_id) {
      return &state;
    }
    if (!empty_slot && state.actor_id == 0) {
      empty_slot = &state;
    }
  }
  return empty_slot;
}

void unpack_enemy_state(MPEnemyState& state, const MPEnemyStatePacked& incoming, uint32_t current_time) {
  state.actor_id = incoming.actor_id;
  state.x = incoming.x;
  state.y = incoming.y;
  state.z = incoming.z;
  state.quat_x = mp_unpack_float_q(incoming.quat[0]);
  state.quat_y = mp_unpack_float_q(incoming.quat[1]);
  state.quat_z = mp_unpack_float_q(incoming.quat[2]);
  state.quat_w = mp_unpack_float_q(incoming.quat[3]);
  state.hp = incoming.hp;
  state.state = incoming.state;
  state.focus_aid = incoming.focus_aid;
  state.attack_flag = (incoming.flags & 1) ? 1 : 0;
  state.owner = (incoming.flags & 2) ? 1 : 0;
  state.is_aggro = (incoming.flags & 4) ? 1 : 0;
  state.last_updated = current_time;
}
}

void mp_handle_enemy_sync_packet(MultiplayerData& data, const ENetPacket* packet, uint32_t current_time) {
  const auto* sync = PacketView(packet).as_minimum<PacketEnemySync>(
      PacketType::ENEMY_SYNC, sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t));
  if (!sync) {
    return;
  }

  uint32_t enemy_count = mp_clamp_count(sync->count, MAX_ENEMIES_PER_PACKET);
  if (!PacketView(packet).has_counted_payload(enemy_count,
                                              sizeof(MPEnemyStatePacked),
                                              sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t))) {
    return;
  }

  data.last_enemy_sync_time = current_time;
  for (uint32_t i = 0; i < enemy_count; i++) {
    const auto& incoming = sync->enemies[i];
    if (incoming.actor_id == 0) {
      continue;
    }
    MPEnemyState* slot = find_enemy_slot(data.remote_enemy_buffer, incoming.actor_id);
    if (slot) {
      unpack_enemy_state(*slot, incoming, current_time);
    }
  }
  data.remote_enemy_buffer.remote_count = MAX_ENEMY_SYNC_COUNT;
}

void mp_send_enemy_sync(MultiplayerData& data, MPEnemySyncBufferGOAL* buffer) {
  if (!buffer || buffer->local_count == 0) {
    return;
  }

  uint32_t total_count = mp_clamp_count(buffer->local_count, MAX_ENEMY_SYNC_COUNT);
  uint32_t sent_count = 0;
  while (sent_count < total_count) {
    uint32_t chunk_size = mp_clamp_count(total_count - sent_count, MAX_ENEMIES_PER_PACKET);
    PacketEnemySync packet = {};
    packet.header.type = PacketType::ENEMY_SYNC;
    packet.header.sequenceNum = ++data.sequence_num;
    packet.count = chunk_size;
    packet.timestamp = enet_time_get();
    for (uint32_t i = 0; i < chunk_size; i++) {
      auto* src = &buffer->local_enemies[sent_count + i];
      auto* dst = &packet.enemies[i];
      dst->actor_id = src->actor_id;
      dst->x = src->x;
      dst->y = src->y;
      dst->z = src->z;
      dst->quat[0] = mp_pack_float_q(src->quat_x);
      dst->quat[1] = mp_pack_float_q(src->quat_y);
      dst->quat[2] = mp_pack_float_q(src->quat_z);
      dst->quat[3] = mp_pack_float_q(src->quat_w);
      dst->hp = src->hp;
      dst->state = src->state;
      dst->focus_aid = src->focus_aid;
      dst->flags = (src->attack_flag ? 1 : 0) | (src->owner ? 2 : 0) |
                   (src->is_aggro ? 4 : 0);
    }
    MultiplayerManager::broadcast(data,
                                  data.local_role,
                                  &packet,
                                  enemy_packet_size(chunk_size),
                                  ENET_PACKET_FLAG_UNSEQUENCED);
    sent_count += chunk_size;
  }
}

void mp_receive_enemy_sync(MultiplayerData& data, MPEnemySyncBufferGOAL* buffer) {
  if (!buffer) {
    return;
  }
  buffer->remote_count = data.remote_enemy_buffer.remote_count;
  memcpy(buffer->remote_enemies,
         data.remote_enemy_buffer.remote_enemies,
         sizeof(MPEnemyState) * MAX_ENEMY_SYNC_COUNT);
  buffer->last_sync_time = data.last_enemy_sync_time;
}
