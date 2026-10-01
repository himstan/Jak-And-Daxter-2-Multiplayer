#include "game/multiplayer/platform/nat/port_mapping_coordinator.h"

#include <chrono>
#include <utility>

namespace multiplayer::platform {
namespace {
constexpr auto kRefreshInterval = std::chrono::hours(1);
constexpr auto kRefreshRetryDelay = std::chrono::seconds(5);
constexpr int kRefreshAttempts = 3;
}  // namespace

PortMappingCoordinator::~PortMappingCoordinator() {
  stop();
}

void PortMappingCoordinator::start(const uint16_t local_port, const uint16_t external_port) {
  stop();
  {
    std::lock_guard lock(mutex_);
    stopping_ = false;
    snapshot_ = {.state = MPPortMappingState::PENDING,
                 .local_port = local_port,
                 .external_port = external_port};
  }
  worker_ = std::thread([this, local_port, external_port] { run(local_port, external_port); });
}

void PortMappingCoordinator::stop() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();

  if (worker_.joinable()) {
    worker_.join();
  }

  std::lock_guard lock(mutex_);
  stopping_ = false;
  snapshot_ = {};
}

PortMappingSnapshot PortMappingCoordinator::snapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

bool PortMappingCoordinator::wait_for_stop(const std::chrono::milliseconds duration) {
  std::unique_lock lock(mutex_);
  return cv_.wait_for(lock, duration, [this] { return stopping_; });
}

void PortMappingCoordinator::run(const uint16_t local_port, const uint16_t external_port) {
  if (wait_for_stop(std::chrono::seconds(1))) {
    return;
  }

  auto mapping = open_port_mapping(local_port, external_port);
  const bool usable =
      mapping.success && !mapping.external_ip.empty() && !is_private_ipv4(mapping.external_ip);

  bool stopped = false;
  {
    std::lock_guard lock(mutex_);
    stopped = stopping_;
    if (!stopped) {
      snapshot_.state = usable ? MPPortMappingState::READY : MPPortMappingState::FAILED;
      snapshot_.method = usable ? mapping.method : MPPortMappingMethod::NONE;
      snapshot_.external_address = usable ? mapping.external_ip : std::string{};
      snapshot_.error = usable ? std::string{} : mapping.error;
    }
  }

  if (stopped || !usable) {
    if (mapping.success) {
      close_udp_port_mapping(mapping, local_port, external_port);
    }
    return;
  }

  if (mapping.method == MPPortMappingMethod::UPNP_IGD) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return stopping_; });
    lock.unlock();
    close_udp_port_mapping(mapping, local_port, external_port);
    return;
  }

  while (!wait_for_stop(std::chrono::duration_cast<std::chrono::milliseconds>(kRefreshInterval))) {
    bool refreshed = false;
    std::string error;

    for (int attempt = 0; attempt < kRefreshAttempts; ++attempt) {
      auto result = refresh_udp_port_mapping(mapping, local_port, external_port);
      if (result.success) {
        refreshed = true;
        break;
      }

      error = std::move(result.error);
      if (attempt + 1 < kRefreshAttempts && wait_for_stop(kRefreshRetryDelay)) {
        break;
      }
    }

    if (!refreshed) {
      std::lock_guard lock(mutex_);
      if (!stopping_) {
        snapshot_.state = MPPortMappingState::FAILED;
        snapshot_.method = MPPortMappingMethod::NONE;
        snapshot_.external_address.clear();
        snapshot_.error = std::move(error);
      }
      break;
    }
  }

  close_udp_port_mapping(mapping, local_port, external_port);
}

}  // namespace multiplayer::platform
