#include "multiplayer_packet.h"

#include "common/log/log.h"
#include "game/multiplayer/multiplayer_types.h"
#include "game/multiplayer/multiplayer_session.h"
#include "game/multiplayer/multiplayer_protocol.h"

#include <cmath>
#include <limits>

int16_t mp_pack_float_q(float value) {
  if (!std::isfinite(value)) {
    return 0;
  }
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

int16_t mp_pack_float_scaled(float value, float scale) {
  if (!std::isfinite(value) || !std::isfinite(scale)) {
    return 0;
  }
  const float scaled = value * scale;
  const float minimum = static_cast<float>((std::numeric_limits<int16_t>::min)());
  const float maximum = static_cast<float>((std::numeric_limits<int16_t>::max)());
  if (scaled <= minimum) {
    return (std::numeric_limits<int16_t>::min)();
  }
  if (scaled >= maximum) {
    return (std::numeric_limits<int16_t>::max)();
  }
  return static_cast<int16_t>(scaled);
}

bool mp_float_is_finite(float value) {
  return std::isfinite(value);
}

uint32_t mp_clamp_count(uint32_t count, uint32_t max_count) {
  return (count < max_count) ? count : max_count;
}

size_t mp_counted_packet_size(uint32_t count, size_t element_size) {
  constexpr size_t prefix_size = sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint64_t);
  if (element_size != 0 &&
      count > ((std::numeric_limits<size_t>::max)() - prefix_size) / element_size) {
    return 0;
  }
  return prefix_size + (element_size * count);
}

bool mp_sequence_is_newer(uint32_t incoming, uint32_t previous) {
  return previous == 0 || static_cast<int32_t>(incoming - previous) > 0;
}

PacketView::PacketView(const ENetPacket* packet) : m_packet(packet) {}

bool PacketView::has_header() const {
  return m_packet && m_packet->data && m_packet->dataLength >= sizeof(PacketHeader);
}

PacketType PacketView::type() const {
  if (!has_header()) {
    return PacketType::COUNT;
  }
  PacketType packet_type = PacketType::COUNT;
  memcpy(&packet_type, m_packet->data, sizeof(packet_type));
  return packet_type;
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
  if (element_size != 0 &&
      count > ((std::numeric_limits<size_t>::max)() - prefix_size) / element_size) {
    return false;
  }
  return m_packet->dataLength == prefix_size + (element_size * count);
}

bool mp_packet_direction_allowed(PacketType type, int sender_role) {
  if ((sender_role != 0 && sender_role != 1) || type >= PacketType::COUNT) {
    return false;
  }
  switch (type) {
    case PacketType::FULL_SYNC:
    case PacketType::PEDESTRIAN_SYNC:
    case PacketType::VEHICLE_SYNC:
    case PacketType::PALACE_SQUID_SYNC:
      return sender_role == 0;
    case PacketType::STATE_UPDATE:
    case PacketType::EVENT_GAME:
    case PacketType::ENEMY_SYNC:
    case PacketType::TURRET_SYNC:
    case PacketType::AIRLOCK_SYNC:
      return true;
    case PacketType::EVENT_JOIN:
    case PacketType::EVENT_LEAVE:
    case PacketType::COUNT:
      return false;
  }
  return false;
}

bool mp_send_packet(MultiplayerData& data,
                    int channel,
                    const void* packet_data,
                    size_t size,
                    ENetPacketFlag flags) {
  if (!data.host || !packet_data || size == 0) {
    return false;
  }

  PacketType packet_type = PacketType::COUNT;
  if (size < sizeof(PacketHeader)) {
    return false;
  }
  memcpy(&packet_type, packet_data, sizeof(packet_type));
  MultiplayerDatagram secured;
  if (!data.security.seal(data.local_role, packet_type, packet_data, size, secured)) {
    return false;
  }
  channel = (flags & ENET_PACKET_FLAG_RELIABLE)
                ? static_cast<int>(MultiplayerChannel::CONTROL)
                : static_cast<int>(MultiplayerChannel::STATE);

  ENetPacket* packet = enet_packet_create(secured.bytes.data(), secured.size, flags);
  if (!packet) {
    lg::error("[Multiplayer] Failed to allocate ENet packet ({} bytes).", size);
    return false;
  }

  if (data.local_role == 0) {
    if (data.authenticated_peer &&
        data.authenticated_peer->state == ENET_PEER_STATE_CONNECTED &&
        enet_peer_send(data.authenticated_peer, channel, packet) == 0) {
      data.stats.track_sent_bytes(packet_data, size);
      return true;
    }
    enet_packet_destroy(packet);
    return false;
  }

  if (data.server_peer && data.server_peer->state == ENET_PEER_STATE_CONNECTED) {
    if (enet_peer_send(data.server_peer, channel, packet) == 0) {
      data.stats.track_sent_bytes(packet_data, size);
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
  return mp_send_raw_packet_to_peer(peer, channel, packet_data, size, flags);
}

bool mp_send_raw_packet_to_peer(ENetPeer* peer,
                                int channel,
                                const void* packet_data,
                                size_t size,
                                ENetPacketFlag flags) {
  if (!peer || peer->state != ENET_PEER_STATE_CONNECTED || !packet_data || size == 0 ||
      size > kMultiplayerMaxDatagramSize) {
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
