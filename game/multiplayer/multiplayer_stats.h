#pragma once

#include "game/multiplayer/multiplayer_protocol.h"

#include <array>
#include <cstdint>
#include <cstddef>

struct _ENetHost;
struct _ENetPeer;

struct MultiplayerStats {
  static constexpr size_t kPacketTypeCount = static_cast<size_t>(PacketType::COUNT);
  uint32_t last_rate_update_time = 0;
  uint32_t last_sent_bytes = 0;
  uint32_t last_recv_bytes = 0;
  uint32_t last_sent_packets = 0;
  uint32_t last_recv_packets = 0;
  uint32_t send_rate_bytes_per_sec = 0;
  uint32_t recv_rate_bytes_per_sec = 0;
  uint32_t send_rate_packets_per_sec = 0;
  uint32_t recv_rate_packets_per_sec = 0;

  uint32_t last_wire_sent_bytes = 0;
  uint32_t last_wire_recv_bytes = 0;
  uint32_t last_wire_sent_packets = 0;
  uint32_t last_wire_recv_packets = 0;
  uint64_t wire_total_sent_bytes = 0;
  uint64_t wire_total_recv_bytes = 0;
  uint64_t wire_total_sent_packets = 0;
  uint64_t wire_total_recv_packets = 0;
  bool rate_clock_initialized = false;
  bool wire_counter_initialized = false;

  std::array<uint64_t, kPacketTypeCount> sent_bytes_by_type = {};
  std::array<uint64_t, kPacketTypeCount> recv_bytes_by_type = {};
  std::array<uint64_t, kPacketTypeCount> sent_packets_by_type = {};
  std::array<uint64_t, kPacketTypeCount> recv_packets_by_type = {};
  std::array<uint32_t, kPacketTypeCount> send_rate_by_type = {};
  std::array<uint32_t, kPacketTypeCount> recv_rate_by_type = {};
  std::array<uint32_t, kPacketTypeCount> send_packet_rate_by_type = {};
  std::array<uint32_t, kPacketTypeCount> recv_packet_rate_by_type = {};
  std::array<uint64_t, kPacketTypeCount> last_sent_bytes_by_type = {};
  std::array<uint64_t, kPacketTypeCount> last_recv_bytes_by_type = {};
  std::array<uint64_t, kPacketTypeCount> last_sent_packets_by_type = {};
  std::array<uint64_t, kPacketTypeCount> last_recv_packets_by_type = {};

  void calculate_rates(struct _ENetHost* host);
  void calculate_rates(struct _ENetHost* host, uint32_t current_time);
  void track_sent_packet(PacketType type, size_t size);
  void track_recv_packet(PacketType type, size_t size);
  void track_sent_bytes(const void* packet_data, size_t size);
  void track_recv_bytes(const void* packet_data, size_t size);
  void reset();

  static uint64_t counter_delta(uint32_t current, uint32_t previous);
};

bool multiplayer_stats_valid_packet_type(int type);
float multiplayer_enet_ratio_to_percent(uint32_t ratio);
bool multiplayer_enet_rtt_sample_valid(const _ENetPeer& peer);
