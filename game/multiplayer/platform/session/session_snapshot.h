#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/multiplayer/platform/core/network_statistics.h"
#include "game/multiplayer/platform/protocol/session_protocol.h"
#include "game/multiplayer/platform/session/session_state.h"

namespace multiplayer::platform {

struct SessionConnectionSnapshot {
  PlayerId participant = kInvalidPlayerId;
  ConnectionSnapshot network;
};

struct SessionSnapshot {
  SessionState state;
  std::vector<ParticipantProfile> participants;
  std::vector<SessionConnectionSnapshot> connections;
  bool countdown_active = false;
  uint64_t countdown_target_ms = 0;
  AggregateSnapshot statistics;
  RejectionReason rejection = RejectionReason::NONE;
  uint8_t close_reason = 0;
  std::string required_identity;
};

}  // namespace multiplayer::platform
