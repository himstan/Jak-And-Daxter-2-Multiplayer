#include "game/multiplayer/platform/session/cadence_scheduler.h"

namespace multiplayer::platform {

bool CadenceScheduler::due(const GameMessagePolicy& policy,
                           const uint64_t now_ms,
                           const bool dirty,
                           const NetworkPressure pressure) {
  if (policy.cadence == CadenceMode::DIRTY)
    return dirty;
  if (policy.cadence == CadenceMode::ON_DEMAND)
    return true;
  auto& deadline = deadlines_[policy.id];
  if (deadline != 0 && now_ms < deadline)
    return false;
  uint32_t numerator = 1;
  uint32_t denominator = 1;
  if (policy.priority == MessagePriority::BULK) {
    switch (pressure) {
      case NetworkPressure::DEGRADED:
        numerator = 3;
        denominator = 2;
        break;
      case NetworkPressure::CONGESTED:
        numerator = 3;
        break;
      case NetworkPressure::SEVERE:
        numerator = 5;
        break;
      default:
        break;
    }
  } else if (policy.priority == MessagePriority::NORMAL && pressure >= NetworkPressure::CONGESTED) {
    numerator = 2;
  }
  deadline = now_ms + (policy.interval_ms * numerator + denominator - 1) / denominator;
  return true;
}

void CadenceScheduler::reset() {
  deadlines_ = {};
}

NetworkPressure NetworkPressureTracker::update(const NetworkPressure observed,
                                               const uint64_t now_ms) {
  if (observed == pressure_) {
    candidate_ = observed;
    candidate_since_ms_ = now_ms;
    if (pressure_ == NetworkPressure::SEVERE && !severe_active_) {
      severe_since_ms_ = now_ms;
      severe_active_ = true;
    }
    return pressure_;
  }
  if (candidate_ != observed) {
    candidate_ = observed;
    candidate_since_ms_ = now_ms;
    return pressure_;
  }
  if (const uint64_t dwell_ms = observed > pressure_ ? 250 : 3000;
      now_ms - candidate_since_ms_ >= dwell_ms) {
    pressure_ = observed;
    candidate_since_ms_ = now_ms;
    severe_since_ms_ = pressure_ == NetworkPressure::SEVERE ? now_ms : 0;
    severe_active_ = pressure_ == NetworkPressure::SEVERE;
  }
  return pressure_;
}

void NetworkPressureTracker::reset() {
  pressure_ = NetworkPressure::NORMAL;
  candidate_ = NetworkPressure::NORMAL;
  candidate_since_ms_ = 0;
  severe_since_ms_ = 0;
  severe_active_ = false;
}

}  // namespace multiplayer::platform
