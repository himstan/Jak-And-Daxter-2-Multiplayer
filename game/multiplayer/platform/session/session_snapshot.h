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
  bool countdown_active = false;
  uint64_t countdown_target_ms = 0;
  AggregateSnapshot statistics;
  RejectionReason rejection = RejectionReason::NONE;
  uint8_t close_reason = 0;
  std::string required_identity;

  std::optional<int> player_ping_ms(const PlayerId player_id) const {
    if (state.role == SessionRole::NONE || player_id == kInvalidPlayerId)
      return std::nullopt;
    if (player_id == state.local_player_id)
      return 0;
    for (const auto& [conn_player_id, conn_network] : connections) {
      if (conn_player_id == player_id && conn_network.ping_ms >= 0)
        return conn_network.ping_ms;
    }
    return std::nullopt;
  }
};

}  // namespace multiplayer::platform
