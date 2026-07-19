#pragma once

#include "game/multiplayer/multiplayer_protocol.h"

#include <array>
#include <cstdint>
#include <cstddef>

struct _ENetHost;

struct MultiplayerStats {
  static constexpr size_t kPacketTypeCount = static_cast<size_t>(PacketType::COUNT);
  // Global rates and totals
  uint32_t last_rate_update_time = 0;
  uint32_t last_sent_bytes = 0;
  uint32_t last_recv_bytes = 0;
  uint32_t send_rate_bytes_per_sec = 0;
  uint32_t recv_rate_bytes_per_sec = 0;

  // Category rates and totals
  std::array<uint64_t, kPacketTypeCount> sent_bytes_by_type = {};
  std::array<uint64_t, kPacketTypeCount> recv_bytes_by_type = {};
  std::array<uint32_t, kPacketTypeCount> send_rate_by_type = {};
  std::array<uint32_t, kPacketTypeCount> recv_rate_by_type = {};
  std::array<uint64_t, kPacketTypeCount> last_sent_bytes_by_type = {};
  std::array<uint64_t, kPacketTypeCount> last_recv_bytes_by_type = {};

  void calculate_rates(struct _ENetHost* host);
  void track_sent_bytes(const void* packet_data, size_t size);
  void track_recv_bytes(const void* packet_data, size_t size);
  void reset();
};
