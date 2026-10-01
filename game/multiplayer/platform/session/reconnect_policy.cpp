#include "game/multiplayer/platform/session/reconnect_policy.h"

#include <algorithm>
#include <array>

namespace multiplayer::platform {
namespace {
constexpr std::array<uint32_t, 5> kDelaysMs = {250, 500, 1000, 2000, 5000};
}

uint32_t reconnect_delay_ms(const uint8_t attempt_count) {
  return kDelaysMs[std::min<size_t>(attempt_count, kDelaysMs.size() - 1)];
}

void schedule_reconnect(ReconnectState& state, const uint64_t now_ms) {
  state.attempt_active = false;
  state.waiting_for_bootstrap = false;
  state.next_attempt_time_ms = now_ms + reconnect_delay_ms(state.attempt_count);
}

bool reconnect_due(const ReconnectState& state, const uint64_t now_ms) {
  return !state.attempt_active && state.next_attempt_time_ms != 0 &&
         now_ms >= state.next_attempt_time_ms;
}

void mark_reconnect_attempt_started(ReconnectState& state) {
  state.attempt_active = true;
  state.waiting_for_bootstrap = false;
  state.next_attempt_time_ms = 0;
}

void mark_reconnect_attempt_failed(ReconnectState& state, const uint64_t now_ms) {
  state.attempt_active = false;
  state.waiting_for_bootstrap = false;
  if (state.attempt_count + 1 < kDelaysMs.size())
    ++state.attempt_count;
  state.next_attempt_time_ms = now_ms + reconnect_delay_ms(state.attempt_count);
}

void mark_reconnect_authenticated(ReconnectState& state) {
  const bool reconnecting = state.attempt_active || state.waiting_for_bootstrap;
  state.attempt_active = false;
  state.waiting_for_bootstrap = reconnecting;
  if (!reconnecting)
    state.attempt_count = 0;
  state.next_attempt_time_ms = 0;
}

void reset_reconnect(ReconnectState& state) {
  state = {};
}

}  // namespace multiplayer::platform
