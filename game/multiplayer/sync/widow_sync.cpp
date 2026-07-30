#include "widow_sync.h"

#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"

#include <cstring>

void mp_handle_widow_sync_packet(MultiplayerData& data,
                                 const ENetPacket* packet,
                                 uint32_t current_time) {
  const auto sync = PacketView(packet).as_exact<PacketWidowSync>(PacketType::WIDOW_SYNC);
  if (!sync || data.local_role == 0 || sync->state.active > 1 || sync->state.state_id > 25 ||
      !mp_float_is_finite(sync->state.x) || !mp_float_is_finite(sync->state.y) ||
      !mp_float_is_finite(sync->state.z) ||
      !mp_sequence_is_newer(sync->header.sequenceNum, data.last_widow_sequence)) {
    return;
  }

  data.last_widow_sequence = sync->header.sequenceNum;
  memcpy(&data.remote_widow_state, &sync->state, sizeof(MPWidowState));
  data.remote_widow_state.last_updated = current_time;
  data.last_widow_sync_time = current_time;
}

void mp_send_widow_sync(MultiplayerData& data, MPWidowSyncBufferGOAL* buffer) {
  if (!buffer || buffer->local_state.active == 0 || data.local_role != 0) {
    return;
  }

  PacketWidowSync packet = {};
  packet.header.type = PacketType::WIDOW_SYNC;
  packet.header.sequenceNum = ++data.sequence_num;
  packet.timestamp = enet_time_get();
  memcpy(&packet.state, &buffer->local_state, sizeof(MPWidowState));
  packet.state.last_updated = packet.timestamp;
  MultiplayerManager::broadcast(data, data.local_role, packet, ENET_PACKET_FLAG_UNSEQUENCED);
}

void mp_receive_widow_sync(MultiplayerData& data, MPWidowSyncBufferGOAL* buffer) {
  if (!buffer) {
    return;
  }
  memcpy(&buffer->remote_state, &data.remote_widow_state, sizeof(MPWidowState));
  buffer->last_sync_time = data.last_widow_sync_time;
}
