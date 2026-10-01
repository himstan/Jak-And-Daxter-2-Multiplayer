#include "game/multiplayer/platform/session/session_state.h"

namespace multiplayer::platform {

bool valid_player_limit(const uint32_t player_limit) {
  return player_limit >= 2 && player_limit < kInvalidPlayerId;
}

bool can_enter_game(const SessionState& session) {
  return session.status == SessionStatus::LOBBY && session.role != SessionRole::NONE &&
         valid_player_limit(session.player_limit) && session.local_player_id != kInvalidPlayerId &&
         session.host_player_id != kInvalidPlayerId;
}

bool should_retry_connection(const SessionState& session) {
  return session.status == SessionStatus::RECONNECTING && session.reconnect_attempt_active &&
         !session.waiting_for_bootstrap;
}

void mark_connected(SessionState& session,
                    const PlayerId local_player_id,
                    const PlayerId host_player_id) {
  if (local_player_id >= session.player_limit || host_player_id >= session.player_limit) {
    mark_failed(session);
    return;
  }
  session.local_player_id = local_player_id;
  session.host_player_id = host_player_id;
  session.status = SessionStatus::LOBBY;
  session.reconnect_attempt_active = false;
  session.waiting_for_bootstrap = false;
}

void mark_reconnecting(SessionState& session) {
  session.status = SessionStatus::RECONNECTING;
  session.reconnect_attempt_active = true;
  session.waiting_for_bootstrap = false;
}

void mark_failed(SessionState& session) {
  session.status = SessionStatus::FAILED;
  session.reconnect_attempt_active = false;
  session.waiting_for_bootstrap = false;
}

void reset(SessionState& session) {
  session = {};
}

}  // namespace multiplayer::platform
