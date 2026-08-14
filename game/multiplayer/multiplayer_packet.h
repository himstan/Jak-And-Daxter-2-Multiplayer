#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#include "enet/enet.h"
#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/multiplayer_types.h"

int16_t mp_pack_float_q(float value);
float mp_unpack_float_q(int16_t value);
int16_t mp_pack_float_scaled(float value, float scale);
bool mp_float_is_finite(float value);

uint32_t mp_clamp_count(uint32_t count, uint32_t max_count);
size_t mp_counted_packet_size(uint32_t count, size_t element_size);
bool mp_sequence_is_newer(uint32_t incoming, uint32_t previous);
bool mp_sequence_is_current_or_newer(uint32_t incoming, uint32_t previous);
bool mp_packet_direction_allowed(PacketType type, int sender_role);

class PacketView {
 public:
  explicit PacketView(const ENetPacket* packet);

  bool has_header() const;
  PacketType type() const;
  uint32_t sequence_num() const;
  size_t size() const;

  template <typename T>
  std::optional<T> as_exact(PacketType expected_type) const {
    if (!m_packet || m_packet->dataLength != sizeof(T) || !has_header() ||
        type() != expected_type) {
      return std::nullopt;
    }
    T result = {};
    memcpy(&result, m_packet->data, sizeof(T));
    return result;
  }

  template <typename T>
  const T* as_minimum(PacketType expected_type, size_t minimum_size) const {
    if (!m_packet || m_packet->dataLength < minimum_size || !has_header() ||
        type() != expected_type) {
      return nullptr;
    }
    return reinterpret_cast<const T*>(m_packet->data);
  }

  bool has_counted_payload(uint32_t count, size_t element_size, size_t prefix_size) const;
  const uint8_t* data() const;

 private:
  const ENetPacket* m_packet = nullptr;
};

bool mp_send_packet(MultiplayerData& data,
                    int channel,
                    const void* packet_data,
                    size_t size,
                    ENetPacketFlag flags);
bool mp_queue_packet_to_peer(MultiplayerData& data,
                             ENetPeer* peer,
                             int channel,
                             const void* packet_data,
                             size_t size,
                             ENetPacketFlag flags,
                             std::optional<uint32_t> stream_key_override = std::nullopt);
bool mp_send_packet_immediately(MultiplayerData& data,
                                ENetPeer* peer,
                                int channel,
                                const void* packet_data,
                                size_t size,
                                ENetPacketFlag flags);
size_t mp_flush_packet_window(MultiplayerData& data);
bool mp_send_raw_packet_to_peer(ENetPeer* peer,
                                int channel,
                                const void* packet_data,
                                size_t size,
                                ENetPacketFlag flags);
