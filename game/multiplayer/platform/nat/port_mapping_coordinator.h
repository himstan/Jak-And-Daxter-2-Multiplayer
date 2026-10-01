#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "game/multiplayer/platform/nat/port_mapping.h"

namespace multiplayer::platform {

enum class MPPortMappingState {
  IDLE,
  PENDING,
  READY,
  FAILED,
};

struct PortMappingSnapshot {
  MPPortMappingState state = MPPortMappingState::IDLE;
  MPPortMappingMethod method = MPPortMappingMethod::NONE;
  uint16_t local_port = 0;
  uint16_t external_port = 0;
  std::string external_address;
  std::string error;
};

class PortMappingCoordinator {
 public:
  PortMappingCoordinator() = default;
  ~PortMappingCoordinator();
  PortMappingCoordinator(const PortMappingCoordinator&) = delete;
  PortMappingCoordinator& operator=(const PortMappingCoordinator&) = delete;

  void start(uint16_t local_port, uint16_t external_port);
  void stop();
  PortMappingSnapshot snapshot() const;

 private:
  bool wait_for_stop(std::chrono::milliseconds duration);
  void run(uint16_t local_port, uint16_t external_port);

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::thread worker_;
  bool stopping_ = false;
  PortMappingSnapshot snapshot_;
};

}  // namespace multiplayer::platform
