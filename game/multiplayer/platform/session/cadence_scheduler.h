#pragma once

#include <array>
#include <cstdint>

#include "game/multiplayer/platform/core/message_policy.h"

namespace multiplayer::platform {

class CadenceScheduler {
 public:
  bool due(const GameMessagePolicy& policy,
           uint64_t now_ms,
           bool dirty = true,
           NetworkPressure pressure = NetworkPressure::NORMAL);
  void reset();

 private:
  std::array<uint64_t, 256> deadlines_ = {};
};

class NetworkPressureTracker {
 public:
  NetworkPressure update(NetworkPressure observed, uint64_t now_ms);
  void reset();
  NetworkPressure pressure() const { return pressure_; }
  uint64_t severe_since_ms() const { return severe_since_ms_; }

 private:
  NetworkPressure pressure_ = NetworkPressure::NORMAL;
  NetworkPressure candidate_ = NetworkPressure::NORMAL;
  uint64_t candidate_since_ms_ = 0;
  uint64_t severe_since_ms_ = 0;
  bool severe_active_ = false;
};

}  // namespace multiplayer::platform
