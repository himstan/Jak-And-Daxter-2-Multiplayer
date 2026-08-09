#include "enemy_sync.h"

#include <cstring>

#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/sync/player_sync.h"

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

void unpack_enemy_state(MPEnemyState& state,
                        const MPEnemyStatePacked& incoming,
                        uint32_t current_time) {
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
  state.focus_player_id = incoming.focus_player_id;
  state.attack_flag = (incoming.flags & 1) ? 1 : 0;
  state.owner_player_id = incoming.owner_player_id;
  state.is_aggro = (incoming.flags & 2) ? 1 : 0;
  state.last_updated = current_time;
}
}  // namespace

bool mp_handle_enemy_sync_packet(MultiplayerData& data,
                                 const ENetPacket* packet,
                                 uint32_t sender_player_id,
                                 uint32_t current_time) {
  constexpr size_t prefix_size = sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t);
  PacketView view(packet);
  if (!view.has_header() || view.type() != PacketType::ENEMY_SYNC ||
      packet->dataLength < prefix_size) {
    return false;
  }

  uint32_t encoded_count = 0;
  PacketHeader header = {};
  memcpy(&header, packet->data, sizeof(header));
  memcpy(&encoded_count, packet->data + sizeof(PacketHeader), sizeof(encoded_count));
  if (encoded_count > MAX_ENEMIES_PER_PACKET ||
      !view.has_counted_payload(encoded_count, sizeof(MPEnemyStatePacked), prefix_size) ||
      !mp_valid_player_id(sender_player_id) ||
      !mp_sequence_is_newer(header.sequenceNum,
                            data.last_enemy_sequence_by_player[sender_player_id])) {
    return false;
  }

  for (uint32_t i = 0; i < encoded_count; i++) {
    MPEnemyStatePacked incoming = {};
    memcpy(&incoming, packet->data + prefix_size + i * sizeof(incoming), sizeof(incoming));
    if (incoming.actor_id == 0 ||
        (incoming.owner_player_id != kMPInvalidCompactPlayerId &&
         incoming.owner_player_id >= kMPMaxPlayers) ||
        (data.session_role == 0 && incoming.owner_player_id != kMPInvalidCompactPlayerId &&
         incoming.owner_player_id != sender_player_id) ||
        (incoming.focus_player_id != kMPInvalidPlayerId &&
         !mp_valid_player_id(incoming.focus_player_id)) ||
        !mp_float_is_finite(incoming.x) || !mp_float_is_finite(incoming.y) ||
        !mp_float_is_finite(incoming.z)) {
      return false;
    }
  }

  data.last_enemy_sequence_by_player[sender_player_id] = header.sequenceNum;
  data.last_enemy_sync_time = current_time;
  for (uint32_t i = 0; i < encoded_count; i++) {
    MPEnemyStatePacked incoming = {};
    memcpy(&incoming, packet->data + prefix_size + i * sizeof(incoming), sizeof(incoming));
    MPEnemyState* slot = find_enemy_slot(data.remote_enemy_buffer, incoming.actor_id);
    if (slot) {
      unpack_enemy_state(*slot, incoming, current_time);
    }
  }
  data.remote_enemy_buffer.remote_count = MAX_ENEMY_SYNC_COUNT;
  return true;
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
      dst->focus_player_id = src->focus_player_id;
      dst->flags = (src->attack_flag ? 1 : 0) | (src->is_aggro ? 2 : 0);
      dst->owner_player_id = src->owner_player_id;
    }
    MultiplayerManager::broadcast(data, data.session_role, &packet, enemy_packet_size(chunk_size),
                                  ENET_PACKET_FLAG_UNSEQUENCED);
    sent_count += chunk_size;
  }
}

void mp_receive_enemy_sync(MultiplayerData& data, MPEnemySyncBufferGOAL* buffer) {
  if (!buffer) {
    return;
  }
  buffer->remote_count = data.remote_enemy_buffer.remote_count;
  memcpy(buffer->remote_enemies, data.remote_enemy_buffer.remote_enemies,
         sizeof(MPEnemyState) * MAX_ENEMY_SYNC_COUNT);
  buffer->last_sync_time = data.last_enemy_sync_time;
}
