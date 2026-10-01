#include "game/multiplayer/platform/session/participant_registry.h"

#include <algorithm>

#include "game/multiplayer/platform/session/session_state.h"

namespace multiplayer::platform {

ParticipantRegistry::ParticipantRegistry(const uint8_t player_limit) {
  configure(player_limit);
}

bool ParticipantRegistry::configure(const uint8_t player_limit) {
  if (!valid_player_limit(player_limit) || accepted_count() != 0) {
    return false;
  }
  player_limit_ = player_limit;
  entries_.assign(static_cast<size_t>(player_limit - 1), {});
  return true;
}

size_t ParticipantRegistry::accepted_count() const {
  return static_cast<size_t>(std::ranges::count_if(
      entries_, [](const auto& participant) { return participant.accepted; }));
}

bool ParticipantRegistry::has_open_remote_slot() const {
  return accepted_count() + 1 < player_limit_;
}

ParticipantSession* ParticipantRegistry::find_connection(const ConnectionId connection) {
  if (connection == 0)
    return nullptr;
  const auto found = std::ranges::find(entries_, connection, &ParticipantSession::connection_id);
  return found == entries_.end() ? nullptr : &*found;
}

const ParticipantSession* ParticipantRegistry::find_connection(
    const ConnectionId connection) const {
  if (connection == 0)
    return nullptr;
  const auto found = std::ranges::find(entries_, connection, &ParticipantSession::connection_id);
  return found == entries_.end() ? nullptr : &*found;
}

ParticipantSession* ParticipantRegistry::find_player(const PlayerId player) {
  const auto found = std::ranges::find_if(entries_, [player](const auto& participant) {
    return participant.accepted && participant.player_id == player;
  });
  return found == entries_.end() ? nullptr : &*found;
}

const ParticipantSession* ParticipantRegistry::find_player(const PlayerId player) const {
  const auto found = std::ranges::find_if(entries_, [player](const auto& participant) {
    return participant.accepted && participant.player_id == player;
  });
  return found == entries_.end() ? nullptr : &*found;
}

ParticipantSession* ParticipantRegistry::bind(const ConnectionId connection,
                                              const PlayerId player,
                                              const PlayerCharacter character) {
  if (connection == 0 || player >= player_limit_ || find_connection(connection) ||
      find_player(player))
    return nullptr;
  const auto free =
      std::ranges::find(entries_, ConnectionId{0}, &ParticipantSession::connection_id);
  if (free == entries_.end())
    return nullptr;
  *free = {.connection_id = connection,
           .player_id = player,
           .character = character,
           .profile = {.participant = player, .character = character},
           .accepted = true};
  return &*free;
}

ParticipantSession* ParticipantRegistry::allocate(const ConnectionId connection,
                                                  const PlayerCharacter character) {
  if (connection == 0 || find_connection(connection))
    return nullptr;
  for (PlayerId player = 1; player < player_limit_; ++player) {
    if (!find_player(player))
      return bind(connection, player, character);
  }
  return nullptr;
}

bool ParticipantRegistry::release_connection(const ConnectionId connection) {
  auto* participant = find_connection(connection);
  if (!participant)
    return false;
  release(*participant);
  return true;
}

void ParticipantRegistry::release(ParticipantSession& participant) {
  participant = {};
}

void ParticipantRegistry::reset() {
  std::ranges::fill(entries_, ParticipantSession{});
}

void ParticipantRegistry::request_bootstrap_for_all(const std::optional<uint32_t> generation) {
  for (auto& participant : entries_) {
    if (!participant.accepted || !participant.identity_ready)
      continue;
    participant.bootstrap_pending = true;
    participant.bootstrap_sent_once = false;
    participant.last_bootstrap_send_time = 0;
    if (generation) {
      participant.bootstrap_generation = *generation;
    } else {
      ++participant.bootstrap_generation;
      if (participant.bootstrap_generation == 0)
        participant.bootstrap_generation = 1;
    }
    participant.bootstrap_payload.clear();
  }
}

bool ParticipantRegistry::acknowledge_bootstrap(const PlayerId player, const uint32_t generation) {
  auto* participant = find_player(player);
  if (!participant || !participant->bootstrap_pending || generation == 0 ||
      participant->bootstrap_generation != generation)
    return false;
  participant->bootstrap_pending = false;
  participant->bootstrap_sent_once = false;
  participant->last_bootstrap_send_time = 0;
  participant->bootstrap_payload.clear();
  return true;
}

}  // namespace multiplayer::platform
