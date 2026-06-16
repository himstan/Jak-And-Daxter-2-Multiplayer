#pragma once

#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/multiplayer_types.h"

#include "enet/enet.h"

#include <cstddef>
#include <cstdint>

int16_t mp_pack_float_q(float value);
float mp_unpack_float_q(int16_t value);

uint32_t mp_clamp_count(uint32_t count, uint32_t max_count);
size_t mp_counted_packet_size(uint32_t count, size_t element_size);

class PacketView {
 public:
  explicit PacketView(const ENetPacket* packet);

  bool has_header() const;
  PacketType type() const;
  size_t size() const;

  template <typename T>
  const T* as_exact(PacketType expected_type) const {
    if (!m_packet || m_packet->dataLength != sizeof(T) || !has_header() ||
        header()->type != expected_type) {
      return nullptr;
    }
    return reinterpret_cast<const T*>(m_packet->data);
  }

  template <typename T>
  const T* as_minimum(PacketType expected_type, size_t minimum_size) const {
    if (!m_packet || m_packet->dataLength < minimum_size || !has_header() ||
        header()->type != expected_type) {
      return nullptr;
    }
    return reinterpret_cast<const T*>(m_packet->data);
  }

  bool has_counted_payload(uint32_t count, size_t element_size, size_t prefix_size) const;

 private:
  const PacketHeader* header() const;

  const ENetPacket* m_packet = nullptr;
};

bool mp_send_packet(MultiplayerData& data,
                    int channel,
                    const void* packet_data,
                    size_t size,
                    ENetPacketFlag flags);
bool mp_send_packet_to_peer(ENetPeer* peer,
                            int channel,
                            const void* packet_data,
                            size_t size,
                            ENetPacketFlag flags);
