#include "game/multiplayer/platform/session/player_registry.h"

#include <algorithm>

#include "game/multiplayer/platform/session/session_state.h"

namespace multiplayer::platform {

PlayerRegistry::PlayerRegistry(const uint8_t player_limit) {
  configure(player_limit);
}

bool PlayerRegistry::configure(const uint8_t player_limit) {
  if (!valid_player_limit(player_limit) || accepted_count() != 0) {
    return false;
  }
  player_limit_ = player_limit;
  entries_.assign(static_cast<size_t>(player_limit - 1), {});
  return true;
}

size_t PlayerRegistry::accepted_count() const {
  return static_cast<size_t>(
      std::ranges::count_if(entries_, [](const auto& player) { return player.accepted; }));
}

bool PlayerRegistry::has_open_remote_slot() const {
  return accepted_count() + 1 < player_limit_;
}

PlayerSession* PlayerRegistry::find_connection(const ConnectionId connection) {
  if (connection == 0)
    return nullptr;
  const auto found = std::ranges::find(entries_, connection, &PlayerSession::connection_id);
  return found == entries_.end() ? nullptr : &*found;
}

const PlayerSession* PlayerRegistry::find_connection(const ConnectionId connection) const {
  if (connection == 0)
    return nullptr;
  const auto found = std::ranges::find(entries_, connection, &PlayerSession::connection_id);
  return found == entries_.end() ? nullptr : &*found;
}

PlayerSession* PlayerRegistry::find_player(const PlayerId player_id) {
  const auto found = std::ranges::find_if(entries_, [player_id](const auto& player) {
    return player.accepted && player.player_id == player_id;
  });
  return found == entries_.end() ? nullptr : &*found;
}

const PlayerSession* PlayerRegistry::find_player(const PlayerId player_id) const {
  const auto found = std::ranges::find_if(entries_, [player_id](const auto& player) {
    return player.accepted && player.player_id == player_id;
  });
  return found == entries_.end() ? nullptr : &*found;
}

PlayerSession* PlayerRegistry::bind(const ConnectionId connection,
                                    const PlayerId player_id,
                                    const PlayerCharacter character) {
  if (connection == 0 || player_id >= player_limit_ || find_connection(connection) ||
      find_player(player_id))
    return nullptr;
  const auto free = std::ranges::find(entries_, ConnectionId{0}, &PlayerSession::connection_id);
  if (free == entries_.end())
    return nullptr;
  *free = {.connection_id = connection,
           .player_id = player_id,
           .character = character,
           .profile = {.player_id = player_id, .character = character},
           .accepted = true};
  return &*free;
}

PlayerSession* PlayerRegistry::allocate(const ConnectionId connection,
                                        const PlayerCharacter character) {
  if (connection == 0 || find_connection(connection))
    return nullptr;
  for (PlayerId player_id = 1; player_id < player_limit_; ++player_id) {
    if (!find_player(player_id))
      return bind(connection, player_id, character);
  }
  return nullptr;
}

bool PlayerRegistry::release_connection(const ConnectionId connection) {
  auto* player = find_connection(connection);
  if (!player)
    return false;
  release(*player);
  return true;
}

void PlayerRegistry::release(PlayerSession& player_session) {
  player_session = {};
}

void PlayerRegistry::reset() {
  std::ranges::fill(entries_, PlayerSession{});
}

void PlayerRegistry::request_bootstrap_for_all(const std::optional<uint32_t> generation) {
  for (auto& player : entries_) {
    if (!player.accepted || !player.identity_ready)
      continue;
    player.bootstrap_pending = true;
    player.bootstrap_sent_once = false;
    player.last_bootstrap_send_time = 0;
    if (generation) {
      player.bootstrap_generation = *generation;
    } else {
      ++player.bootstrap_generation;
      if (player.bootstrap_generation == 0)
        player.bootstrap_generation = 1;
    }
    player.bootstrap_payload.clear();
  }
}

bool PlayerRegistry::acknowledge_bootstrap(const PlayerId player_id, const uint32_t generation) {
  auto* player = find_player(player_id);
  if (!player || !player->bootstrap_pending || generation == 0 ||
      player->bootstrap_generation != generation)
    return false;
  player->bootstrap_pending = false;
  player->bootstrap_sent_once = false;
  player->last_bootstrap_send_time = 0;
  player->bootstrap_payload.clear();
  return true;
}

}  // namespace multiplayer::platform
