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

enum class LobbyPrivacy : uint8_t { PUBLIC, FRIENDS_ONLY, INVITE_ONLY };

struct SessionState {
  SessionRole role = SessionRole::NONE;
  SessionStatus status = SessionStatus::IDLE;
  PlayerSlot local_player_slot = kInvalidPlayerSlot;
  PlayerSlot host_player_slot = kInvalidPlayerSlot;
  uint8_t player_limit = 8;
  bool reconnect_attempt_active = false;
  bool waiting_for_bootstrap = false;
};

bool valid_player_limit(uint32_t player_limit);
bool can_enter_game(const SessionState& session);
bool should_retry_connection(const SessionState& session);
void mark_connected(SessionState& session,
                    PlayerSlot local_player_slot,
                    PlayerSlot host_player_slot);
void mark_reconnecting(SessionState& session);
void mark_failed(SessionState& session);
void reset(SessionState& session);

}  // namespace multiplayer::platform
