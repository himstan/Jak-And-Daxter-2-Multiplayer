#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "game/multiplayer/platform/discovery/discovery_protocol.h"

namespace multiplayer::platform {

enum class DiscoveryStatus : uint8_t {
  IDLE,
  SEARCHING,
  FOUND,
  TIMED_OUT,
  FAILED,
};

struct DiscoveryConfig {
  uint16_t discovery_port = 0;
  uint16_t expected_game_port = 0;
  std::string game_id;
  std::string compatibility_identity;
  std::string directed_address;
  bool include_full_sessions = false;
};

struct DiscoveryResult {
  std::string invite;
  DiscoveryAdvertisement advertisement;
};

bool discovery_advertisement_matches(const DiscoveryConfig& config,
                                     const DiscoveryAdvertisement& advertisement);

class DiscoveryScanner {
 public:
  ~DiscoveryScanner();

  bool start(DiscoveryConfig config);
  void stop();
  DiscoveryStatus status() const { return status_; }
  std::optional<DiscoveryResult> take_result();

 private:
  void scan(DiscoveryConfig config);

  std::atomic<bool> stopping_{false};
  std::atomic<DiscoveryStatus> status_{DiscoveryStatus::IDLE};
  std::mutex result_mutex_;
  std::optional<DiscoveryResult> result_;
  std::thread thread_;
};

class DiscoveryResponder {
 public:
  ~DiscoveryResponder();

  bool start(uint16_t discovery_port, DiscoveryAdvertisement advertisement);
  void update(DiscoveryAdvertisement advertisement);
  void stop();
  bool active() const { return active_; }

 private:
  void respond(uint16_t discovery_port);

  std::atomic<bool> active_{false};
  std::mutex advertisement_mutex_;
  DiscoveryAdvertisement advertisement_;
  std::thread thread_;
  uint16_t discovery_port_ = 0;
};

}  // namespace multiplayer::platform
