#pragma once

#include <optional>
#include <vector>

#include "game/multiplayer/platform/core/types.h"
#include "game/multiplayer/platform/protocol/session_protocol.h"

namespace multiplayer::platform {

struct PlayerSession {
  static constexpr uint8_t kControlBudgetMaximum = 32;

  ConnectionId connection_id = 0;
  PlayerSlot player_slot = kInvalidPlayerSlot;
  PlayerCharacter character = PlayerCharacter::UNKNOWN;
  PlayerProfile profile;
  std::string remote_address;
  uint64_t steam_id = 0;
  std::vector<uint8_t> bootstrap_payload;
  uint64_t last_bootstrap_send_time = 0;
  uint64_t control_budget_updated_ms = 0;
  uint32_t bootstrap_generation = 0;
  uint8_t control_budget = kControlBudgetMaximum;
  bool accepted = false;
  bool identity_ready = false;
  bool bootstrap_pending = false;
  bool bootstrap_sent_once = false;
};

class PlayerRegistry {
 public:
  explicit PlayerRegistry(uint8_t player_limit = 8);

  bool configure(uint8_t player_limit);
  uint8_t player_limit() const { return player_limit_; }
  size_t accepted_count() const;
  bool has_open_remote_slot() const;

  PlayerSession* find_connection(ConnectionId connection);
  const PlayerSession* find_connection(ConnectionId connection) const;
  PlayerSession* find_player(PlayerSlot player_slot);
  const PlayerSession* find_player(PlayerSlot player_slot) const;
  PlayerSession* bind(ConnectionId connection, PlayerSlot player_slot, PlayerCharacter character);
  PlayerSession* allocate(ConnectionId connection, PlayerCharacter character);
  bool release_connection(ConnectionId connection);
  static void release(PlayerSession& player_session);
  void reset();
  void request_bootstrap_for_all(std::optional<uint32_t> generation = std::nullopt);
  bool acknowledge_bootstrap(PlayerSlot player_slot, uint32_t generation);

  std::vector<PlayerSession>& entries() { return entries_; }
  const std::vector<PlayerSession>& entries() const { return entries_; }

 private:
  uint8_t player_limit_ = 0;
  std::vector<PlayerSession> entries_;
};

}  // namespace multiplayer::platform
