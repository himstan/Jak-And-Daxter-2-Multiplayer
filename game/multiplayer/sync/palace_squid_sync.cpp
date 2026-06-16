#include "palace_squid_sync.h"

#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"

#include <cstring>

void mp_handle_palace_squid_sync_packet(MultiplayerData& data,
                                        const ENetPacket* packet,
                                        uint32_t current_time) {
  const auto* sync =
      PacketView(packet).as_exact<PacketPalaceSquidSync>(PacketType::PALACE_SQUID_SYNC);
  if (!sync || data.local_role == 0) {
    return;
  }

  memcpy(&data.remote_palace_squid_state, &sync->state, sizeof(MPPalaceSquidState));
  data.remote_palace_squid_state.last_updated = current_time;
  data.last_palace_squid_sync_time = current_time;
}

void mp_send_palace_squid_sync(MultiplayerData& data, MPPalaceSquidSyncBufferGOAL* buffer) {
  if (!buffer || buffer->local_state.active == 0 || data.local_role != 0) {
    return;
  }

  PacketPalaceSquidSync packet = {};
  packet.header.type = PacketType::PALACE_SQUID_SYNC;
  packet.header.sequenceNum = ++data.sequence_num;
  packet.timestamp = enet_time_get();
  memcpy(&packet.state, &buffer->local_state, sizeof(MPPalaceSquidState));
  packet.state.last_updated = packet.timestamp;
  MultiplayerManager::broadcast(data, data.local_role, packet, ENET_PACKET_FLAG_UNSEQUENCED);
}

void mp_receive_palace_squid_sync(MultiplayerData& data, MPPalaceSquidSyncBufferGOAL* buffer) {
  if (!buffer) {
    return;
  }
  memcpy(&buffer->remote_state, &data.remote_palace_squid_state, sizeof(MPPalaceSquidState));
  buffer->last_sync_time = data.last_palace_squid_sync_time;
}
