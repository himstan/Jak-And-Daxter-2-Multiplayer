#include "airlock_sync.h"

#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"

#include <cstring>

void mp_handle_airlock_sync_packet(MultiplayerData& data,
                                   const ENetPacket* packet,
                                   uint32_t current_time) {
  const auto* sync = PacketView(packet).as_exact<PacketAirlockSync>(PacketType::AIRLOCK_SYNC);
  if (!sync || sync->sequence <= data.last_remote_airlock_sequence) {
    return;
  }

  memset(&data.remote_airlock_table, 0, sizeof(data.remote_airlock_table));
  data.remote_airlock_table.count =
      sync->count < (uint32_t)MAX_AIRLOCK_SYNC_COUNT ? sync->count : MAX_AIRLOCK_SYNC_COUNT;
  for (uint32_t i = 0; i < data.remote_airlock_table.count; i++) {
    memcpy(&data.remote_airlock_table.states[i], &sync->states[i], sizeof(MPAirlockStateGOAL));
    data.remote_airlock_table.states[i].last_updated = current_time;
  }
  data.last_remote_airlock_sequence = sync->sequence;
  data.last_airlock_sync_time = current_time;
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
  MultiplayerManager::broadcast(data, data.local_role, packet, ENET_PACKET_FLAG_UNSEQUENCED);
}

void mp_receive_airlock_sync(MultiplayerData& data, MPAirlockSyncBufferGOAL* buffer) {
  if (!buffer) {
    return;
  }

  memcpy(&buffer->remote_table, &data.remote_airlock_table, sizeof(MPAirlockStateTableGOAL));
  buffer->last_sync_time = data.last_airlock_sync_time;
}
