#include "multiplayer_stats.h"
#include "enet/enet.h"
#include "common/log/log.h"
#include "multiplayer_protocol.h"
#include "multiplayer_session.h"

#include <cstring>

void MultiplayerStats::reset() {
  last_rate_update_time = 0;
  last_sent_bytes = 0;
  last_recv_bytes = 0;
  send_rate_bytes_per_sec = 0;
  recv_rate_bytes_per_sec = 0;
  for (size_t i = 0; i < kPacketTypeCount; i++) {
    sent_bytes_by_type[i] = 0;
    recv_bytes_by_type[i] = 0;
    send_rate_by_type[i] = 0;
    recv_rate_by_type[i] = 0;
    last_sent_bytes_by_type[i] = 0;
    last_recv_bytes_by_type[i] = 0;
  }
}

void MultiplayerStats::calculate_rates(struct _ENetHost* host) {
  if (!host) {
    send_rate_bytes_per_sec = 0;
    recv_rate_bytes_per_sec = 0;
    last_sent_bytes = 0;
    last_recv_bytes = 0;
    last_rate_update_time = 0;
    for (size_t i = 0; i < kPacketTypeCount; i++) {
      send_rate_by_type[i] = 0;
      recv_rate_by_type[i] = 0;
      last_sent_bytes_by_type[i] = 0;
      last_recv_bytes_by_type[i] = 0;
    }
    return;
  }

  uint32_t current_time = enet_time_get();
  if (last_rate_update_time == 0) {
    last_rate_update_time = current_time;
    last_sent_bytes = host->totalSentData;
    last_recv_bytes = host->totalReceivedData;
    send_rate_bytes_per_sec = 0;
    recv_rate_bytes_per_sec = 0;
    for (size_t i = 0; i < kPacketTypeCount; i++) {
      last_sent_bytes_by_type[i] = sent_bytes_by_type[i];
      last_recv_bytes_by_type[i] = recv_bytes_by_type[i];
      send_rate_by_type[i] = 0;
      recv_rate_by_type[i] = 0;
    }
    return;
  }

  uint32_t elapsed = current_time - last_rate_update_time;
  if (elapsed >= 1000) {
    uint32_t sent = host->totalSentData;
    uint32_t recv = host->totalReceivedData;

    uint32_t sent_diff = (sent >= last_sent_bytes) ? (sent - last_sent_bytes) : sent;
    uint32_t recv_diff = (recv >= last_recv_bytes) ? (recv - last_recv_bytes) : recv;

    send_rate_bytes_per_sec = (uint32_t)((double)sent_diff / ((double)elapsed / 1000.0));
    recv_rate_bytes_per_sec = (uint32_t)((double)recv_diff / ((double)elapsed / 1000.0));

    last_sent_bytes = sent;
    last_recv_bytes = recv;

    // Type-specific rates
    for (size_t i = 0; i < kPacketTypeCount; i++) {
      uint64_t type_sent = sent_bytes_by_type[i];
      uint64_t type_recv = recv_bytes_by_type[i];

      uint64_t type_sent_diff = (type_sent >= last_sent_bytes_by_type[i]) ? (type_sent - last_sent_bytes_by_type[i]) : type_sent;
      uint64_t type_recv_diff = (type_recv >= last_recv_bytes_by_type[i]) ? (type_recv - last_recv_bytes_by_type[i]) : type_recv;

      send_rate_by_type[i] = (uint32_t)((double)type_sent_diff / ((double)elapsed / 1000.0));
      recv_rate_by_type[i] = (uint32_t)((double)type_recv_diff / ((double)elapsed / 1000.0));

      last_sent_bytes_by_type[i] = type_sent;
      last_recv_bytes_by_type[i] = type_recv;
    }

    last_rate_update_time = current_time;
  }
}

void MultiplayerStats::track_sent_bytes(const void* packet_data, size_t size) {
  if (packet_data && size >= sizeof(PacketHeader)) {
    uint8_t type_idx = 0;
    memcpy(&type_idx, packet_data, sizeof(type_idx));
    if (type_idx < kPacketTypeCount) {
      sent_bytes_by_type[type_idx] += size;
    }
  }
}

void MultiplayerStats::track_recv_bytes(const void* packet_data, size_t size) {
  if (packet_data && size >= sizeof(PacketHeader)) {
    uint8_t type_idx = 0;
    memcpy(&type_idx, packet_data, sizeof(type_idx));
    if (type_idx < kPacketTypeCount) {
      recv_bytes_by_type[type_idx] += size;
    }
  }
}
