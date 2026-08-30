#include "multiplayer_stats.h"
#include "enet/enet.h"
#include "common/log/log.h"
#include "multiplayer_protocol.h"
#include "multiplayer_session.h"

#include <cstring>
#include <limits>

namespace {
constexpr uint32_t kRateWindowMilliseconds = 1000;

uint32_t rate_from_delta(uint64_t delta, uint32_t elapsed) {
  if (elapsed == 0) {
    return 0;
  }

  const long double rate = (static_cast<long double>(delta) * 1000.0L) /
                           static_cast<long double>(elapsed);
  if (rate >= static_cast<long double>((std::numeric_limits<uint32_t>::max)())) {
    return (std::numeric_limits<uint32_t>::max)();
  }
  return static_cast<uint32_t>(rate);
}

void clear_rate_state(MultiplayerStats& stats) {
  stats.last_rate_update_time = 0;
  stats.last_sent_bytes = 0;
  stats.last_recv_bytes = 0;
  stats.last_sent_packets = 0;
  stats.last_recv_packets = 0;
  stats.send_rate_bytes_per_sec = 0;
  stats.recv_rate_bytes_per_sec = 0;
  stats.send_rate_packets_per_sec = 0;
  stats.recv_rate_packets_per_sec = 0;
  stats.rate_clock_initialized = false;
  for (size_t i = 0; i < MultiplayerStats::kPacketTypeCount; i++) {
    stats.send_rate_by_type[i] = 0;
    stats.recv_rate_by_type[i] = 0;
    stats.send_packet_rate_by_type[i] = 0;
    stats.recv_packet_rate_by_type[i] = 0;
    stats.last_sent_bytes_by_type[i] = 0;
    stats.last_recv_bytes_by_type[i] = 0;
    stats.last_sent_packets_by_type[i] = 0;
    stats.last_recv_packets_by_type[i] = 0;
  }
}
}  // namespace

uint64_t MultiplayerStats::counter_delta(uint32_t current, uint32_t previous) {
  if (current >= previous) {
    return static_cast<uint64_t>(current - previous);
  }
  return static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)() - previous) + 1u +
         current;
}

bool multiplayer_stats_valid_packet_type(int type) {
  return type >= 0 && static_cast<size_t>(type) < MultiplayerStats::kPacketTypeCount;
}

float multiplayer_enet_ratio_to_percent(uint32_t ratio) {
  return static_cast<float>(static_cast<double>(ratio) * 100.0 / 65536.0);
}

bool multiplayer_enet_rtt_sample_valid(const _ENetPeer& peer) {
  return peer.lastReceiveTime != 0;
}

void MultiplayerStats::reset() {
  last_rate_update_time = 0;
  last_sent_bytes = 0;
  last_recv_bytes = 0;
  last_sent_packets = 0;
  last_recv_packets = 0;
  send_rate_bytes_per_sec = 0;
  recv_rate_bytes_per_sec = 0;
  send_rate_packets_per_sec = 0;
  recv_rate_packets_per_sec = 0;
  last_wire_sent_bytes = 0;
  last_wire_recv_bytes = 0;
  last_wire_sent_packets = 0;
  last_wire_recv_packets = 0;
  wire_total_sent_bytes = 0;
  wire_total_recv_bytes = 0;
  wire_total_sent_packets = 0;
  wire_total_recv_packets = 0;
  rate_clock_initialized = false;
  wire_counter_initialized = false;
  for (size_t i = 0; i < kPacketTypeCount; i++) {
    sent_bytes_by_type[i] = 0;
    recv_bytes_by_type[i] = 0;
    send_rate_by_type[i] = 0;
    recv_rate_by_type[i] = 0;
    send_packet_rate_by_type[i] = 0;
    recv_packet_rate_by_type[i] = 0;
    last_sent_bytes_by_type[i] = 0;
    last_recv_bytes_by_type[i] = 0;
    last_sent_packets_by_type[i] = 0;
    last_recv_packets_by_type[i] = 0;
  }
}

void MultiplayerStats::calculate_rates(struct _ENetHost* host) {
  calculate_rates(host, enet_time_get());
}

void MultiplayerStats::calculate_rates(struct _ENetHost* host, uint32_t current_time) {
  if (!host) {
    clear_rate_state(*this);
    wire_counter_initialized = false;
    last_wire_sent_bytes = 0;
    last_wire_recv_bytes = 0;
    last_wire_sent_packets = 0;
    last_wire_recv_packets = 0;
    return;
  }

  const uint32_t current_sent_bytes = host->totalSentData;
  const uint32_t current_recv_bytes = host->totalReceivedData;
  const uint32_t current_sent_packets = host->totalSentPackets;
  const uint32_t current_recv_packets = host->totalReceivedPackets;

  if (!wire_counter_initialized) {
    last_wire_sent_bytes = current_sent_bytes;
    last_wire_recv_bytes = current_recv_bytes;
    last_wire_sent_packets = current_sent_packets;
    last_wire_recv_packets = current_recv_packets;
    wire_counter_initialized = true;
  } else {
    wire_total_sent_bytes += counter_delta(current_sent_bytes, last_wire_sent_bytes);
    wire_total_recv_bytes += counter_delta(current_recv_bytes, last_wire_recv_bytes);
    wire_total_sent_packets += counter_delta(current_sent_packets, last_wire_sent_packets);
    wire_total_recv_packets += counter_delta(current_recv_packets, last_wire_recv_packets);
    last_wire_sent_bytes = current_sent_bytes;
    last_wire_recv_bytes = current_recv_bytes;
    last_wire_sent_packets = current_sent_packets;
    last_wire_recv_packets = current_recv_packets;
  }

  if (!rate_clock_initialized) {
    last_rate_update_time = current_time;
    last_sent_bytes = current_sent_bytes;
    last_recv_bytes = current_recv_bytes;
    last_sent_packets = current_sent_packets;
    last_recv_packets = current_recv_packets;
    rate_clock_initialized = true;
    send_rate_bytes_per_sec = 0;
    recv_rate_bytes_per_sec = 0;
    send_rate_packets_per_sec = 0;
    recv_rate_packets_per_sec = 0;
    for (size_t i = 0; i < kPacketTypeCount; i++) {
      last_sent_bytes_by_type[i] = sent_bytes_by_type[i];
      last_recv_bytes_by_type[i] = recv_bytes_by_type[i];
      last_sent_packets_by_type[i] = sent_packets_by_type[i];
      last_recv_packets_by_type[i] = recv_packets_by_type[i];
      send_rate_by_type[i] = 0;
      recv_rate_by_type[i] = 0;
      send_packet_rate_by_type[i] = 0;
      recv_packet_rate_by_type[i] = 0;
    }
    return;
  }

  uint32_t elapsed = current_time - last_rate_update_time;
  if (elapsed >= kRateWindowMilliseconds) {
    send_rate_bytes_per_sec = rate_from_delta(counter_delta(current_sent_bytes, last_sent_bytes), elapsed);
    recv_rate_bytes_per_sec = rate_from_delta(counter_delta(current_recv_bytes, last_recv_bytes), elapsed);
    send_rate_packets_per_sec =
        rate_from_delta(counter_delta(current_sent_packets, last_sent_packets), elapsed);
    recv_rate_packets_per_sec =
        rate_from_delta(counter_delta(current_recv_packets, last_recv_packets), elapsed);

    last_sent_bytes = current_sent_bytes;
    last_recv_bytes = current_recv_bytes;
    last_sent_packets = current_sent_packets;
    last_recv_packets = current_recv_packets;

    // Type-specific rates
    for (size_t i = 0; i < kPacketTypeCount; i++) {
      uint64_t type_sent = sent_bytes_by_type[i];
      uint64_t type_recv = recv_bytes_by_type[i];

      uint64_t type_sent_diff = type_sent >= last_sent_bytes_by_type[i]
                                    ? type_sent - last_sent_bytes_by_type[i]
                                    : type_sent;
      uint64_t type_recv_diff = type_recv >= last_recv_bytes_by_type[i]
                                    ? type_recv - last_recv_bytes_by_type[i]
                                    : type_recv;
      uint64_t type_sent_packet_diff =
          sent_packets_by_type[i] >= last_sent_packets_by_type[i]
              ? sent_packets_by_type[i] - last_sent_packets_by_type[i]
              : sent_packets_by_type[i];
      uint64_t type_recv_packet_diff =
          recv_packets_by_type[i] >= last_recv_packets_by_type[i]
              ? recv_packets_by_type[i] - last_recv_packets_by_type[i]
              : recv_packets_by_type[i];

      send_rate_by_type[i] = rate_from_delta(type_sent_diff, elapsed);
      recv_rate_by_type[i] = rate_from_delta(type_recv_diff, elapsed);
      send_packet_rate_by_type[i] = rate_from_delta(type_sent_packet_diff, elapsed);
      recv_packet_rate_by_type[i] = rate_from_delta(type_recv_packet_diff, elapsed);

      last_sent_bytes_by_type[i] = type_sent;
      last_recv_bytes_by_type[i] = type_recv;
      last_sent_packets_by_type[i] = sent_packets_by_type[i];
      last_recv_packets_by_type[i] = recv_packets_by_type[i];
    }

    last_rate_update_time = current_time;
  }
}

void MultiplayerStats::track_sent_bytes(const void* packet_data, size_t size) {
  if (packet_data && size >= kPacketHeaderWireSize) {
    const auto type = static_cast<PacketType>(*static_cast<const uint8_t*>(packet_data));
    track_sent_packet(type, size);
  }
}

void MultiplayerStats::track_recv_bytes(const void* packet_data, size_t size) {
  if (packet_data && size >= kPacketHeaderWireSize) {
    const auto type = static_cast<PacketType>(*static_cast<const uint8_t*>(packet_data));
    track_recv_packet(type, size);
  }
}

void MultiplayerStats::track_sent_packet(PacketType type, size_t size) {
  const size_t type_idx = static_cast<size_t>(type);
  if (type_idx >= kPacketTypeCount) {
    return;
  }
  sent_bytes_by_type[type_idx] += size;
  sent_packets_by_type[type_idx]++;
}

void MultiplayerStats::track_recv_packet(PacketType type, size_t size) {
  const size_t type_idx = static_cast<size_t>(type);
  if (type_idx >= kPacketTypeCount) {
    return;
  }
  recv_bytes_by_type[type_idx] += size;
  recv_packets_by_type[type_idx]++;
}
