#pragma once

#include <cstdint>

#include "game/multiplayer/platform/core/types.h"

namespace multiplayer::platform {

enum class SessionStatus : uint8_t {
  IDLE,
  CONNECTING,
  LOBBY,
  GAME_STARTING,
  IN_GAME,
  RECONNECTING,
  HOST_LEFT,
  FAILED,
};

struct SessionState {
  SessionRole role = SessionRole::NONE;
  SessionStatus status = SessionStatus::IDLE;
  PlayerId local_player_id = kInvalidPlayerId;
  PlayerId host_player_id = kInvalidPlayerId;
  uint8_t player_limit = 8;
  bool reconnect_attempt_active = false;
  bool waiting_for_bootstrap = false;
};

bool valid_player_limit(uint32_t player_limit);
bool can_enter_game(const SessionState& session);
bool should_retry_connection(const SessionState& session);
void mark_connected(SessionState& session, PlayerId local_player_id, PlayerId host_player_id);
void mark_reconnecting(SessionState& session);
void mark_failed(SessionState& session);
void reset(SessionState& session);

}  // namespace multiplayer::platform
