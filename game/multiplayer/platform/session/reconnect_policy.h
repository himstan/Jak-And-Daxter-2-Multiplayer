#pragma once

#include <cstdint>

namespace multiplayer::platform {

struct ReconnectState {
  bool attempt_active = false;
  bool waiting_for_bootstrap = false;
  uint8_t attempt_count = 0;
  uint64_t next_attempt_time_ms = 0;
};

uint32_t reconnect_delay_ms(uint8_t attempt_count);
void schedule_reconnect(ReconnectState& state, uint64_t now_ms);
bool reconnect_due(const ReconnectState& state, uint64_t now_ms);
void mark_reconnect_attempt_started(ReconnectState& state);
void mark_reconnect_attempt_failed(ReconnectState& state, uint64_t now_ms);
void mark_reconnect_authenticated(ReconnectState& state);
void reset_reconnect(ReconnectState& state);

}  // namespace multiplayer::platform
