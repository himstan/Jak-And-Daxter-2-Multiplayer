#include "multiplayer_packet.h"

#include "common/log/log.h"
#include "game/multiplayer/generated/multiplayer_schema_generated.h"
#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/multiplayer_security.h"
#include "game/multiplayer/multiplayer_session.h"
#include "game/multiplayer/multiplayer_stats.h"
#include "game/multiplayer/multiplayer_types.h"
#include "game/multiplayer/multiplayer_wire_codec.h"

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
  return static_cast<int16_t>(value * 32767.0f);
}

float mp_unpack_float_q(int16_t value) {
  return static_cast<float>(value) / 32767.0f;
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
  return count < max_count ? count : max_count;
}

size_t mp_counted_packet_size(uint32_t count, size_t element_size) {
  constexpr size_t prefix_size = kPacketHeaderWireSize + sizeof(uint32_t) + sizeof(uint64_t);
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
  return m_packet && m_packet->data && m_packet->dataLength >= kPacketHeaderWireSize;
}

PacketType PacketView::type() const {
  if (!has_header()) {
    return PacketType::COUNT;
  }
  const uint8_t raw_type = m_packet->data[0];
  return raw_type < static_cast<uint8_t>(PacketType::COUNT)
             ? static_cast<PacketType>(raw_type)
             : PacketType::COUNT;
}

uint32_t PacketView::sequence_num() const {
  if (!has_header()) {
    return 0;
  }
  return multiplayer::wire::load_u32_le(m_packet->data + sizeof(uint8_t));
}

size_t PacketView::size() const {
  return m_packet ? m_packet->dataLength : 0;
}

const uint8_t* PacketView::data() const {
  return m_packet ? m_packet->data : nullptr;
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
  const auto* descriptor = multiplayer::schema::packet_descriptor(static_cast<uint8_t>(type));
  return descriptor && (descriptor->allowed_roles & (1u << sender_role)) != 0;
}

bool mp_send_packet(MultiplayerData& data,
                    int channel,
                    const void* packet_data,
                    size_t size,
                    ENetPacketFlag flags) {
  if (!data.host || !packet_data || size < kPacketHeaderWireSize ||
      size > multiplayer::schema::kMaxPacketSize) {
    return false;
  }

  const PacketType packet_type = static_cast<PacketType>(*static_cast<const uint8_t*>(packet_data));
  const auto* descriptor = multiplayer::schema::packet_descriptor(static_cast<uint8_t>(packet_type));
  if (!descriptor || size - kPacketHeaderWireSize > descriptor->max_payload) {
    return false;
  }

  channel = (flags & ENET_PACKET_FLAG_RELIABLE)
                ? static_cast<int>(MultiplayerChannel::CONTROL)
                : static_cast<int>(MultiplayerChannel::STATE);

  ENetPeer* target_peer = nullptr;
  if (data.session_role == 0) {
    target_peer = data.authenticated_peer;
  } else if (data.session_role == 1) {
    target_peer = data.server_peer;
  }

  if (!target_peer || target_peer->state != ENET_PEER_STATE_CONNECTED) {
    return false;
  }

  return data.packet_scheduler.enqueue_plain(
      target_peer, channel, packet_data, size, packet_type, size, flags);
}

bool mp_send_packet_immediately(MultiplayerData& data,
                                ENetPeer* peer,
                                int channel,
                                const void* packet_data,
                                size_t size,
                                ENetPacketFlag flags) {
  if (!data.host || !peer || peer->state != ENET_PEER_STATE_CONNECTED || !packet_data ||
      size < kPacketHeaderWireSize || size > multiplayer::schema::kMaxPacketSize) {
    return false;
  }

  const PacketType packet_type = static_cast<PacketType>(*static_cast<const uint8_t*>(packet_data));
  const auto* descriptor = multiplayer::schema::packet_descriptor(static_cast<uint8_t>(packet_type));
  if (!descriptor || size - kPacketHeaderWireSize > descriptor->max_payload) {
    return false;
  }

  channel = (flags & ENET_PACKET_FLAG_RELIABLE)
                ? static_cast<int>(MultiplayerChannel::CONTROL)
                : static_cast<int>(MultiplayerChannel::STATE);

  MultiplayerDatagram secured;
  if (!data.security.seal(data.session_role, packet_type, packet_data, size, secured)) {
    if (packet_type == PacketType::EVENT_LEAVE) {
      lg::warn("[MP-Leave] Secure seal rejected EVENT_LEAVE (peer_state={}, plaintext_bytes={}).",
               static_cast<int>(peer->state), size);
    }
    return false;
  }

  ENetPacket* packet = enet_packet_create(secured.bytes.data(), secured.size, flags);
  if (!packet) {
    if (packet_type == PacketType::EVENT_LEAVE) {
      lg::warn("[MP-Leave] ENet packet allocation failed for EVENT_LEAVE (secure_bytes={}).",
               secured.size);
    }
    return false;
  }

  const int send_result = enet_peer_send(peer, channel, packet);
  if (packet_type == PacketType::EVENT_LEAVE) {
    lg::info("[MP-Leave] enet_peer_send(EVENT_LEAVE) result={} (channel={}, secure_bytes={}, peer_state={}, reliable_in_transit_before_flush={}, waiting_data={}).",
             send_result, channel, secured.size, static_cast<int>(peer->state),
             peer->reliableDataInTransit, peer->totalWaitingData);
  }
  if (send_result != 0) {
    enet_packet_destroy(packet);
    return false;
  }

  data.stats.track_sent_packet(packet_type, size);
  return true;
}

size_t mp_flush_packet_window(MultiplayerData& data) {
  return data.packet_scheduler.flush_plain(
      data.stats,
      [&data](ENetPeer* peer,
              int channel,
              const uint8_t* plaintext,
              size_t plaintext_size,
              PacketType packet_type,
              size_t,
              ENetPacketFlag flags) {
        if (!peer || peer->state != ENET_PEER_STATE_CONNECTED || !plaintext || plaintext_size == 0) {
          return false;
        }
        MultiplayerDatagram secured;
        if (!data.security.seal(data.session_role,
                                packet_type,
                                plaintext,
                                plaintext_size,
                                secured)) {
          return false;
        }
        ENetPacket* packet = enet_packet_create(secured.bytes.data(), secured.size, flags);
        if (!packet) {
          return false;
        }
        if (enet_peer_send(peer, channel, packet) != 0) {
          enet_packet_destroy(packet);
          return false;
        }
        return true;
      });
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
      size > multiplayer::schema::kMaxPacketSize) {
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
