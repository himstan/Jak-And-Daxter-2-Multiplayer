#ifdef _WIN32

#include "tools/multiplayer_netem/netem_core.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <queue>
#include <random>
#include <utility>

namespace multiplayer_netem {
namespace {

DirectionSettings symmetric_settings(int latency_ms,
                                     int jitter_ms,
                                     double loss_percent,
                                     uint32_t burst_length,
                                     double reorder_percent,
                                     int reorder_delay_ms,
                                     double duplicate_percent) {
  DirectionSettings settings;
  settings.latency_ms = latency_ms;
  settings.jitter_ms = jitter_ms;
  settings.loss_percent = loss_percent;
  settings.burst_length = burst_length;
  settings.reorder_percent = reorder_percent;
  settings.reorder_delay_ms = reorder_delay_ms;
  settings.duplicate_percent = duplicate_percent;
  return settings;
}

NetemProfile make_symmetric_profile(std::string name,
                                    int latency_ms,
                                    int jitter_ms,
                                    double loss_percent,
                                    uint32_t burst_length,
                                    double reorder_percent,
                                    int reorder_delay_ms,
                                    double duplicate_percent) {
  const auto settings = symmetric_settings(latency_ms, jitter_ms, loss_percent, burst_length,
                                            reorder_percent, reorder_delay_ms, duplicate_percent);
  return NetemProfile{std::move(name), settings, settings};
}

}  // namespace

std::optional<NetemProfile> find_profile(std::string_view name) {
  if (name == "lan") {
    return make_symmetric_profile("lan", 1, 0, 0.0, 0, 0.0, 0, 0.0);
  }
  if (name == "wifi") {
    return make_symmetric_profile("wifi", 25, 8, 1.0, 2, 0.5, 15, 0.1);
  }
  if (name == "4g") {
    return make_symmetric_profile("4g", 55, 25, 2.0, 3, 1.0, 30, 0.2);
  }
  if (name == "poor-4g") {
    return make_symmetric_profile("poor-4g", 100, 50, 5.0, 4, 2.0, 60, 0.5);
  }
  if (name == "stress") {
    return make_symmetric_profile("stress", 150, 75, 10.0, 6, 5.0, 100, 1.0);
  }
  return std::nullopt;
}

bool validate_settings(const DirectionSettings& settings, std::string& error) {
  if (settings.latency_ms < 0 || settings.jitter_ms < 0 || settings.reorder_delay_ms < 0) {
    error = "latency, jitter, and reorder delay must be non-negative";
    return false;
  }
  if (!std::isfinite(settings.loss_percent) || !std::isfinite(settings.reorder_percent) ||
      !std::isfinite(settings.duplicate_percent) || settings.loss_percent < 0.0 ||
      settings.loss_percent > 100.0 || settings.reorder_percent < 0.0 ||
      settings.reorder_percent > 100.0 || settings.duplicate_percent < 0.0 ||
      settings.duplicate_percent > 100.0) {
    error = "loss, reorder, and duplicate percentages must be between 0 and 100";
    return false;
  }
  return true;
}

class ImpairmentModel::RandomSource {
 public:
  explicit RandomSource(uint64_t seed) : generator(seed) {}

  std::mt19937_64 generator;
};

ImpairmentModel::ImpairmentModel(DirectionSettings settings, uint64_t seed)
    : m_settings(settings), m_random(std::make_unique<RandomSource>(seed)) {}

ImpairmentModel::~ImpairmentModel() = default;

ImpairmentModel::ImpairmentModel(ImpairmentModel&&) noexcept = default;

ImpairmentModel& ImpairmentModel::operator=(ImpairmentModel&&) noexcept = default;

bool ImpairmentModel::roll_percent(double percent) {
  if (percent <= 0.0) {
    return false;
  }
  if (percent >= 100.0) {
    return true;
  }
  std::uniform_real_distribution<double> distribution(0.0, 100.0);
  return distribution(m_random->generator) < percent;
}

uint32_t ImpairmentModel::calculate_delay_ms() {
  int delay_ms = m_settings.latency_ms;
  if (m_settings.jitter_ms > 0) {
    std::uniform_int_distribution<int> distribution(-m_settings.jitter_ms, m_settings.jitter_ms);
    delay_ms += distribution(m_random->generator);
  }
  return static_cast<uint32_t>(std::max(delay_ms, 0));
}

ImpairmentResult ImpairmentModel::apply() {
  ImpairmentResult result;
  if (m_burst_remaining > 0) {
    --m_burst_remaining;
    result.dropped = true;
    return result;
  }

  if (roll_percent(m_settings.loss_percent)) {
    m_burst_remaining = m_settings.burst_length;
    result.dropped = true;
    return result;
  }

  result.delay_ms = calculate_delay_ms();
  if (roll_percent(m_settings.reorder_percent)) {
    result.delay_ms += static_cast<uint32_t>(m_settings.reorder_delay_ms);
  }
  result.duplicated = roll_percent(m_settings.duplicate_percent);
  return result;
}

struct PacketQueue::CompareReleaseTime {
  bool operator()(const QueuedDatagram& left, const QueuedDatagram& right) const {
    if (left.release_at != right.release_at) {
      return left.release_at > right.release_at;
    }
    return left.sequence > right.sequence;
  }
};

PacketQueue::PacketQueue(size_t max_packets, size_t max_bytes)
    : m_max_packets(max_packets), m_max_bytes(max_bytes) {}

bool PacketQueue::push(QueuedDatagram datagram) {
  if (datagram.payload.empty() || datagram.payload.size() > m_max_bytes ||
      m_packets.size() >= m_max_packets || m_byte_count > m_max_bytes - datagram.payload.size()) {
    return false;
  }

  datagram.sequence = m_next_sequence++;
  m_byte_count += datagram.payload.size();
  m_packets.push_back(std::move(datagram));
  std::push_heap(m_packets.begin(), m_packets.end(), CompareReleaseTime{});
  return true;
}

std::vector<QueuedDatagram> PacketQueue::take_due(NetemTimePoint now) {
  std::vector<QueuedDatagram> due;
  while (!m_packets.empty() && m_packets.front().release_at <= now) {
    std::pop_heap(m_packets.begin(), m_packets.end(), CompareReleaseTime{});
    due.push_back(std::move(m_packets.back()));
    m_byte_count -= due.back().payload.size();
    m_packets.pop_back();
  }
  return due;
}

size_t PacketQueue::packet_count() const {
  return m_packets.size();
}

size_t PacketQueue::byte_count() const {
  return m_byte_count;
}

}  // namespace multiplayer_netem

#endif
