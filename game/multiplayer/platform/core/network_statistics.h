#pragma once

#include <algorithm>
#include <span>

#include "game/multiplayer/platform/core/message_policy.h"
#include "game/multiplayer/platform/core/types.h"

namespace multiplayer::platform {

struct ConnectionSnapshot {
  ConnectionId connection_id = 0;
  int ping_ms = 0;
  int jitter_us = 0;
  float local_quality = -1.0f;
  float remote_quality = -1.0f;
  float send_bytes_per_second = 0.0f;
  float receive_bytes_per_second = 0.0f;
  int send_rate_bytes_per_second = 0;
  int pending_unreliable_bytes = 0;
  int pending_reliable_bytes = 0;
  int sent_unacked_reliable_bytes = 0;
  int critical_queue_time_us = 0;
  int normal_queue_time_us = 0;
  int bulk_queue_time_us = 0;
};

struct AggregateSnapshot {
  float send_bytes_per_second = 0.0f;
  float receive_bytes_per_second = 0.0f;
  int send_rate_bytes_per_second = 0;
  int pending_unreliable_bytes = 0;
  int pending_reliable_bytes = 0;
  int sent_unacked_reliable_bytes = 0;
  int worst_ping_ms = 0;
  int worst_jitter_us = 0;
  float minimum_local_quality = -1.0f;
  float minimum_remote_quality = -1.0f;
  int worst_critical_queue_time_us = 0;
  int worst_normal_queue_time_us = 0;
  int worst_bulk_queue_time_us = 0;
};

inline AggregateSnapshot aggregate_connection_snapshots(
    const std::span<const ConnectionSnapshot> connections) {
  AggregateSnapshot result;
  for (const auto& connection : connections) {
    result.send_bytes_per_second += connection.send_bytes_per_second;
    result.receive_bytes_per_second += connection.receive_bytes_per_second;
    result.send_rate_bytes_per_second += connection.send_rate_bytes_per_second;
    result.pending_unreliable_bytes += connection.pending_unreliable_bytes;
    result.pending_reliable_bytes += connection.pending_reliable_bytes;
    result.sent_unacked_reliable_bytes += connection.sent_unacked_reliable_bytes;
    result.worst_ping_ms = std::max(result.worst_ping_ms, connection.ping_ms);
    result.worst_jitter_us = std::max(result.worst_jitter_us, connection.jitter_us);
    result.worst_critical_queue_time_us =
        std::max(result.worst_critical_queue_time_us, connection.critical_queue_time_us);
    result.worst_normal_queue_time_us =
        std::max(result.worst_normal_queue_time_us, connection.normal_queue_time_us);
    result.worst_bulk_queue_time_us =
        std::max(result.worst_bulk_queue_time_us, connection.bulk_queue_time_us);
    if (connection.local_quality >= 0.0f) {
      result.minimum_local_quality =
          result.minimum_local_quality < 0.0f
              ? connection.local_quality
              : std::min(result.minimum_local_quality, connection.local_quality);
    }
    if (connection.remote_quality >= 0.0f) {
      result.minimum_remote_quality =
          result.minimum_remote_quality < 0.0f
              ? connection.remote_quality
              : std::min(result.minimum_remote_quality, connection.remote_quality);
    }
  }
  return result;
}

inline NetworkPressure classify_network_pressure(const AggregateSnapshot& snapshot) {
  const float utilization =
      snapshot.send_rate_bytes_per_second > 0
          ? snapshot.send_bytes_per_second / static_cast<float>(snapshot.send_rate_bytes_per_second)
          : 0.0f;
  const float quality =
      snapshot.minimum_local_quality < 0.0f ? snapshot.minimum_remote_quality
      : snapshot.minimum_remote_quality < 0.0f
          ? snapshot.minimum_local_quality
          : std::min(snapshot.minimum_local_quality, snapshot.minimum_remote_quality);
  const int queue_time_us =
      std::max({snapshot.worst_critical_queue_time_us, snapshot.worst_normal_queue_time_us,
                snapshot.worst_bulk_queue_time_us});
  if ((quality >= 0.0f && quality < 0.5f) || utilization >= 0.95f || queue_time_us >= 500000)
    return NetworkPressure::SEVERE;
  if ((quality >= 0.0f && quality < 0.75f) || utilization >= 0.85f || queue_time_us >= 250000)
    return NetworkPressure::CONGESTED;
  if ((quality >= 0.0f && quality < 0.9f) || utilization >= 0.7f || queue_time_us >= 100000)
    return NetworkPressure::DEGRADED;
  return NetworkPressure::NORMAL;
}

}  // namespace multiplayer::platform
