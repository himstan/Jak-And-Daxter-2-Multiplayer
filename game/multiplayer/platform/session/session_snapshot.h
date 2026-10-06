#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "game/multiplayer/platform/core/network_statistics.h"
#include "game/multiplayer/platform/protocol/session_protocol.h"
#include "game/multiplayer/platform/session/session_state.h"

namespace multiplayer::platform {

struct SessionConnectionSnapshot {
  PlayerId player_id = kInvalidPlayerId;
  ConnectionSnapshot network;
};

struct SessionSnapshot {
  SessionState state;
  std::vector<PlayerProfile> players;
  std::vector<SessionConnectionSnapshot> connections;
  std::vector<uint16_t> player_pings;
  bool countdown_active = false;
  uint64_t countdown_target_ms = 0;
  AggregateSnapshot statistics;
  RejectionReason rejection = RejectionReason::NONE;
  uint8_t close_reason = 0;
  std::string required_identity;

  std::optional<int> player_ping_ms(const PlayerId player_id) const {
    if (state.role == SessionRole::NONE || player_id == kInvalidPlayerId)
      return std::nullopt;
    if (player_id == state.host_player_id)
      return 0;
    if (player_id < player_pings.size() && player_pings[player_id] != kUnknownPlayerPing)
      return player_pings[player_id];
    return std::nullopt;
  }
};

}  // namespace multiplayer::platform
