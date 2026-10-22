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
  PlayerSlot player_slot = kInvalidPlayerSlot;
  ConnectionSnapshot network;
};

struct ChatMessage {
  uint32_t serial = 0;
  PlayerSlot player_slot = kInvalidPlayerSlot;
  std::string display_name;
  std::string text;
};

struct SessionSnapshot {
  SessionState state;
  std::vector<PlayerProfile> players;
  std::vector<ChatMessage> chat;
  std::vector<SessionConnectionSnapshot> connections;
  std::vector<uint16_t> player_pings;
  bool countdown_active = false;
  uint64_t countdown_target_ms = 0;
  AggregateSnapshot statistics;
  RejectionReason rejection = RejectionReason::NONE;
  uint8_t close_reason = 0;
  std::string required_identity;

  std::optional<int> player_ping_ms(const PlayerSlot player_slot) const {
    if (state.role == SessionRole::NONE || player_slot == kInvalidPlayerSlot)
      return std::nullopt;
    if (player_slot == state.host_player_slot)
      return 0;
    if (player_slot < player_pings.size() && player_pings[player_slot] != kUnknownPlayerPing)
      return player_pings[player_slot];
    return std::nullopt;
  }
};

}  // namespace multiplayer::platform
