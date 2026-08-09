#include "airlock_sync.h"

#include <cstring>

#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/sync/player_sync.h"

bool mp_handle_airlock_sync_packet(MultiplayerData& data,
                                   const ENetPacket* packet,
                                   uint32_t sender_player_id,
                                   uint32_t current_time) {
  const auto sync = PacketView(packet).as_exact<PacketAirlockSync>(PacketType::AIRLOCK_SYNC);
  if (!sync || sync->count > MAX_AIRLOCK_SYNC_COUNT || !mp_valid_player_id(sender_player_id) ||
      !mp_sequence_is_newer(sync->sequence,
                            data.last_airlock_sequence_by_player[sender_player_id])) {
    return false;
  }

  for (uint32_t i = 0; i < sync->count; i++) {
    if (sync->states[i].airlock_aid == 0 || sync->states[i].state_id > 3) {
      return false;
    }
  }
  memset(&data.remote_airlock_table, 0, sizeof(data.remote_airlock_table));
  data.remote_airlock_table.count = sync->count;
  for (uint32_t i = 0; i < data.remote_airlock_table.count; i++) {
    memcpy(&data.remote_airlock_table.states[i], &sync->states[i], sizeof(MPAirlockStateGOAL));
    data.remote_airlock_table.states[i].last_updated = current_time;
  }
  data.last_airlock_sequence_by_player[sender_player_id] = sync->sequence;
  data.last_airlock_sync_time = current_time;
  return true;
}

void mp_send_airlock_sync(MultiplayerData& data, MPAirlockSyncBufferGOAL* buffer) {
  if (!buffer || buffer->sequence == 0) {
    return;
  }

  PacketAirlockSync packet = {};
  packet.header.type = PacketType::AIRLOCK_SYNC;
  packet.header.sequenceNum = ++data.sequence_num;
  packet.count = buffer->local_table.count < (uint32_t)MAX_AIRLOCK_SYNC_COUNT
                     ? buffer->local_table.count
                     : MAX_AIRLOCK_SYNC_COUNT;
  memcpy(packet.states, buffer->local_table.states, sizeof(packet.states));
  packet.sequence = buffer->sequence;
  MultiplayerManager::broadcast(data, data.session_role, packet, ENET_PACKET_FLAG_UNSEQUENCED);
}

void mp_receive_airlock_sync(MultiplayerData& data, MPAirlockSyncBufferGOAL* buffer) {
  if (!buffer) {
    return;
  }

  memcpy(&buffer->remote_table, &data.remote_airlock_table, sizeof(MPAirlockStateTableGOAL));
  buffer->last_sync_time = data.last_airlock_sync_time;
}
