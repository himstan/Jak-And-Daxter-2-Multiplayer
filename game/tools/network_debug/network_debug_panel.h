#pragma once

#include <array>
#include <chrono>

#include "game/multiplayer/platform/runtime/multiplayer_runtime.h"

class NetworkDebugPanel {
 public:
  void draw(const multiplayer::platform::RuntimeSnapshot& snapshot, bool* open);

 private:
  static constexpr size_t kHistorySize = 60 * 4;

  struct HistorySeries {
    std::array<float, kHistorySize> values = {};
    size_t count = 0;
    size_t next = 0;

    void clear();
    void push(float value);
    float at(size_t index) const;
  };

  void reset_history();
  void update_history(const multiplayer::platform::RuntimeSnapshot& snapshot);

  HistorySeries receive_rate_;
  HistorySeries send_rate_;
  HistorySeries ping_;
  HistorySeries jitter_;
  HistorySeries reliable_pressure_;
  std::chrono::steady_clock::time_point last_sample_;
  multiplayer::platform::SessionRole session_role_ = multiplayer::platform::SessionRole::NONE;
  multiplayer::platform::PlayerId local_player_id_ = multiplayer::platform::kInvalidPlayerId;
  multiplayer::platform::PlayerId host_player_id_ = multiplayer::platform::kInvalidPlayerId;
  bool has_session_ = false;
};
