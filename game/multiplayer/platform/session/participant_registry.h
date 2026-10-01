#pragma once

#include <optional>
#include <vector>

#include "game/multiplayer/platform/core/types.h"
#include "game/multiplayer/platform/protocol/session_protocol.h"

namespace multiplayer::platform {

struct ParticipantSession {
  ConnectionId connection_id = 0;
  PlayerId player_id = kInvalidPlayerId;
  PlayerCharacter character = PlayerCharacter::UNKNOWN;
  ParticipantProfile profile;
  std::vector<uint8_t> bootstrap_payload;
  uint64_t last_bootstrap_send_time = 0;
  uint32_t bootstrap_generation = 0;
  bool accepted = false;
  bool identity_ready = false;
  bool bootstrap_pending = false;
  bool bootstrap_sent_once = false;
};

class ParticipantRegistry {
 public:
  explicit ParticipantRegistry(uint8_t player_limit = 8);

  bool configure(uint8_t player_limit);
  uint8_t player_limit() const { return player_limit_; }
  size_t accepted_count() const;
  bool has_open_remote_slot() const;

  ParticipantSession* find_connection(ConnectionId connection);
  const ParticipantSession* find_connection(ConnectionId connection) const;
  ParticipantSession* find_player(PlayerId player);
  const ParticipantSession* find_player(PlayerId player) const;
  ParticipantSession* bind(ConnectionId connection, PlayerId player, PlayerCharacter character);
  ParticipantSession* allocate(ConnectionId connection, PlayerCharacter character);
  bool release_connection(ConnectionId connection);
  static void release(ParticipantSession& participant);
  void reset();
  void request_bootstrap_for_all(std::optional<uint32_t> generation = std::nullopt);
  bool acknowledge_bootstrap(PlayerId player, uint32_t generation);

  std::vector<ParticipantSession>& entries() { return entries_; }
  const std::vector<ParticipantSession>& entries() const { return entries_; }

 private:
  uint8_t player_limit_ = 0;
  std::vector<ParticipantSession> entries_;
};

}  // namespace multiplayer::platform
