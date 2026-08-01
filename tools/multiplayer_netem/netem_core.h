#pragma once

#ifdef _WIN32

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace multiplayer_netem {

using NetemClock = std::chrono::steady_clock;
using NetemTimePoint = NetemClock::time_point;

enum class Direction {
  ClientToHost,
  HostToClient,
};

struct DirectionSettings {
  int latency_ms = 0;
  int jitter_ms = 0;
  double loss_percent = 0.0;
  uint32_t burst_length = 0;
  double reorder_percent = 0.0;
  int reorder_delay_ms = 0;
  double duplicate_percent = 0.0;
};

struct NetemProfile {
  std::string name;
  DirectionSettings client_to_host;
  DirectionSettings host_to_client;
};

struct ImpairmentResult {
  bool dropped = false;
  bool duplicated = false;
  uint32_t delay_ms = 0;
};

std::optional<NetemProfile> find_profile(std::string_view name);
bool validate_settings(const DirectionSettings& settings, std::string& error);

class ImpairmentModel {
 public:
  ImpairmentModel(DirectionSettings settings, uint64_t seed);
  ~ImpairmentModel();

  ImpairmentModel(const ImpairmentModel&) = delete;
  ImpairmentModel& operator=(const ImpairmentModel&) = delete;
  ImpairmentModel(ImpairmentModel&&) noexcept;
  ImpairmentModel& operator=(ImpairmentModel&&) noexcept;

  ImpairmentResult apply();

 private:
  bool roll_percent(double percent);
  uint32_t calculate_delay_ms();

  DirectionSettings m_settings;
  uint64_t m_burst_remaining = 0;
  class RandomSource;
  std::unique_ptr<RandomSource> m_random;
};

struct QueuedDatagram {
  std::vector<uint8_t> payload;
  NetemTimePoint release_at;
  Direction direction = Direction::ClientToHost;
  uint64_t sequence = 0;
};

class PacketQueue {
 public:
  PacketQueue(size_t max_packets, size_t max_bytes);

  bool push(QueuedDatagram datagram);
  std::vector<QueuedDatagram> take_due(NetemTimePoint now);
  size_t packet_count() const;
  size_t byte_count() const;

 private:
  struct CompareReleaseTime;

  size_t m_max_packets;
  size_t m_max_bytes;
  size_t m_byte_count = 0;
  uint64_t m_next_sequence = 0;
  std::vector<QueuedDatagram> m_packets;
};

struct DirectionStats {
  uint64_t received_packets = 0;
  uint64_t received_bytes = 0;
  uint64_t forwarded_packets = 0;
  uint64_t forwarded_bytes = 0;
  uint64_t dropped_packets = 0;
  uint64_t duplicated_packets = 0;
  uint64_t queue_dropped_packets = 0;
};

struct RelayStats {
  DirectionStats client_to_host;
  DirectionStats host_to_client;
  uint64_t ignored_packets = 0;
  uint64_t send_errors = 0;
};

}  // namespace multiplayer_netem

#endif
