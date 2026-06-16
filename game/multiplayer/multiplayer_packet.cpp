#include "multiplayer_packet.h"

#include "common/log/log.h"
#include "game/multiplayer/multiplayer_types.h"

int16_t mp_pack_float_q(float value) {
  if (value > 1.0f) {
    value = 1.0f;
  }
  if (value < -1.0f) {
    value = -1.0f;
  }
  return (int16_t)(value * 32767.0f);
}

float mp_unpack_float_q(int16_t value) {
  return (float)value / 32767.0f;
}

uint32_t mp_clamp_count(uint32_t count, uint32_t max_count) {
  return (count < max_count) ? count : max_count;
}

size_t mp_counted_packet_size(uint32_t count, size_t element_size) {
  return sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t) + (element_size * count);
}

PacketView::PacketView(const ENetPacket* packet) : m_packet(packet) {}

bool PacketView::has_header() const {
  return m_packet && m_packet->data && m_packet->dataLength >= sizeof(PacketHeader);
}

PacketType PacketView::type() const {
  return has_header() ? header()->type : PacketType::STATE_UPDATE;
}

size_t PacketView::size() const {
  return m_packet ? m_packet->dataLength : 0;
}

bool PacketView::has_counted_payload(uint32_t count,
                                     size_t element_size,
                                     size_t prefix_size) const {
  if (!m_packet || !m_packet->data) {
    return false;
  }
  return m_packet->dataLength >= prefix_size + (element_size * count);
}

const PacketHeader* PacketView::header() const {
  return reinterpret_cast<const PacketHeader*>(m_packet->data);
}

bool mp_send_packet(MultiplayerData& data,
                    int channel,
                    const void* packet_data,
                    size_t size,
                    ENetPacketFlag flags) {
  if (!data.host || !packet_data || size == 0) {
    return false;
  }

  ENetPacket* packet = enet_packet_create(packet_data, size, flags);
  if (!packet) {
    lg::error("[Multiplayer] Failed to allocate ENet packet ({} bytes).", size);
    return false;
  }

  if (data.local_role == 0) {
    enet_host_broadcast(data.host, channel, packet);
    return true;
  }

  if (data.server_peer && data.server_peer->state == ENET_PEER_STATE_CONNECTED) {
    if (enet_peer_send(data.server_peer, channel, packet) == 0) {
      return true;
    }
    enet_packet_destroy(packet);
    return false;
  }

  enet_packet_destroy(packet);
  return false;
}

bool mp_send_packet_to_peer(ENetPeer* peer,
                            int channel,
                            const void* packet_data,
                            size_t size,
                            ENetPacketFlag flags) {
  if (!peer || peer->state != ENET_PEER_STATE_CONNECTED || !packet_data || size == 0) {
    return false;
  }

  ENetPacket* packet = enet_packet_create(packet_data, size, flags);
  if (!packet) {
    lg::error("[Multiplayer] Failed to allocate peer packet ({} bytes).", size);
    return false;
  }

  if (enet_peer_send(peer, channel, packet) != 0) {
    enet_packet_destroy(packet);
    return false;
  }
  return true;
}
