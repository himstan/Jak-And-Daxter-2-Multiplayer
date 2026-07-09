#pragma once
#include <cstdint>
#include <cstddef>

struct _ENetHost;

struct MultiplayerStats {
  // Global rates and totals
  uint32_t last_rate_update_time = 0;
  uint32_t last_sent_bytes = 0;
  uint32_t last_recv_bytes = 0;
  uint32_t send_rate_bytes_per_sec = 0;
  uint32_t recv_rate_bytes_per_sec = 0;

  // Category rates and totals
  uint64_t sent_bytes_by_type[11] = {0};
  uint64_t recv_bytes_by_type[11] = {0};
  uint32_t send_rate_by_type[11] = {0};
  uint32_t recv_rate_by_type[11] = {0};
  uint64_t last_sent_bytes_by_type[11] = {0};
  uint64_t last_recv_bytes_by_type[11] = {0};

  void calculate_rates(struct _ENetHost* host);
  void track_sent_bytes(const void* packet_data, size_t size);
  void track_recv_bytes(const void* packet_data, size_t size);
  void reset();
};
