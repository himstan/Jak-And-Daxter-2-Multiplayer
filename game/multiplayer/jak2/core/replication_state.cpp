#include "game/multiplayer/jak2/core/replication_state.h"

#include <algorithm>
#include <cctype>

#include "common/log/log.h"

#include "game/multiplayer/jak2/core/validation.h"
#include "game/multiplayer/platform/core/sequence.h"

namespace multiplayer::jak2::core {
namespace {

bool valid_character(const PlayerCharacter character) {
  return character == PlayerCharacter::JAK || character == PlayerCharacter::DAXTER;
}

bool valid_name(const std::array<uint8_t, kPlayerNameSize>& name) {
  const auto terminator = std::ranges::find(name, 0);
  return terminator != name.end() && std::all_of(name.begin(), terminator, [](const uint8_t value) {
           return std::isalnum(static_cast<unsigned char>(value)) != 0;
         });
}

}  // namespace

void PlayerReplicationState::reset() {
  identities_ = {};
  player_states_ = {};
  player_vehicles_ = {};
  player_vehicle_sequences_ = {};
  turrets_ = {};
  turret_sequences_ = {};
}

void PlayerReplicationState::expire(const uint64_t now_ms) {
  for (PlayerId player_id = 0; player_id < kMaxPlayers; ++player_id) {
    auto& player = player_states_[player_id];
    if (!player.state_ready || now_ms - player.received_time_ms <= 2000)
      continue;
    lg::debug("[MP-PlayerState] Expiring player {}: age={}ms sequence={} activity={} vehicle={}.",
              player_id, now_ms - player.received_time_ms, player.last_sequence,
              static_cast<uint8_t>(player.activity), player.vehicle_id);
    player.state_ready = false;
    identities_[player_id].state_ready = false;
    player_vehicles_[player_id] = {};
    player_vehicle_sequences_[player_id] = 0;
    turrets_[player_id] = {};
    turret_sequences_[player_id] = 0;
  }
}

bool PlayerReplicationState::apply(const PlayerState& state, const ApplyContext& context) {
  if (!valid_index(state.player_id, kMaxPlayers) ||
      !source_allows_player(context.source, state.player_id) || !finite_vector(state.position) ||
      !finite_vector(state.velocity) || !finite(state.angle) || !finite(state.camera_angle_y) ||
      !valid_player_level_state(state.levels) ||
      !platform::sequence_is_newer(context.sequence,
                                   player_states_[state.player_id].last_sequence)) {
    return false;
  }
  auto& current = player_states_[state.player_id];
  if (current.vehicle_id != state.vehicle_id) {
    player_vehicles_[state.player_id] = {};
    player_vehicle_sequences_[state.player_id] = 0;
    turrets_[state.player_id] = {};
    turret_sequences_[state.player_id] = 0;
  }
  current = state;
  current.last_sequence = context.sequence;
  current.received_time_ms = context.received_at_ms;
  player_vehicles_[state.player_id].seat_index = state.vehicle_seat;
  if (identities_[state.player_id].joined) {
    identities_[state.player_id].state_ready = state.state_ready;
    identities_[state.player_id].spectator_only = state.spectator_only;
  }
  if (!state.turret_active) {
    turrets_[state.player_id] = {};
    turret_sequences_[state.player_id] = 0;
  }
  return true;
}

bool PlayerReplicationState::apply(const PlayerVehicleState& state, const ApplyContext& context) {
  if (!valid_index(state.player_id, kMaxPlayers) ||
      !source_allows_player(context.source, state.player_id) ||
      !valid_player_vehicle_state(state.vehicle) ||
      (player_states_[state.player_id].last_sequence != 0 &&
       player_states_[state.player_id].vehicle_id != state.vehicle.net_id) ||
      !platform::sequence_is_newer(context.sequence, player_vehicle_sequences_[state.player_id])) {
    return false;
  }
  auto& player = player_states_[state.player_id];
  player.vehicle_id = state.vehicle.net_id;
  if (player.last_sequence == 0)
    player.vehicle_seat = state.seat_index;
  player_vehicle_sequences_[state.player_id] = context.sequence;
  player_vehicles_[state.player_id] = state;
  player_vehicles_[state.player_id].seat_index = player.vehicle_seat;
  player_vehicles_[state.player_id].vehicle.last_sequence = context.sequence;
  player_vehicles_[state.player_id].vehicle.received_time_ms = context.received_at_ms;
  return true;
}

bool PlayerReplicationState::apply(const TurretState& state, const ApplyContext& context) {
  if (!valid_index(state.player_id, kMaxPlayers) ||
      !source_allows_player(context.source, state.player_id) || !finite(state.rotation_x) ||
      !finite(state.rotation_y) || state.turret_aid == 0 ||
      (player_states_[state.player_id].last_sequence != 0 &&
       (!player_states_[state.player_id].turret_active ||
        player_states_[state.player_id].vehicle_id != state.turret_aid)) ||
      !platform::sequence_is_newer(context.sequence, turret_sequences_[state.player_id])) {
    return false;
  }
  turrets_[state.player_id] = state;
  turret_sequences_[state.player_id] = context.sequence;
  player_states_[state.player_id].turret_active = true;
  return true;
}

bool PlayerReplicationState::update_identity(const PlayerIdentity& identity) {
  if (!valid_index(identity.player_id, kMaxPlayers) || !valid_character(identity.character) ||
      !valid_name(identity.name) || !is_player_appearance_valid(identity.appearance)) {
    return false;
  }
  identities_[identity.player_id] = identity;
  identities_[identity.player_id].identity_ready = true;
  identities_[identity.player_id].joined = true;
  return true;
}

void PlayerReplicationState::depart(const PlayerId player_id) {
  if (!valid_index(player_id, kMaxPlayers))
    return;
  identities_[player_id] = {};
  player_states_[player_id] = {};
  player_vehicles_[player_id] = {};
  player_vehicle_sequences_[player_id] = 0;
  turrets_[player_id] = {};
  turret_sequences_[player_id] = 0;
}

void WorldReplicationState::reset() {
  world_ = {};
  gungame_ = {};
  bootstrap_ = {};
}

bool WorldReplicationState::apply(const WorldState& state, const ApplyContext& context) {
  if (!valid_world_state(state) ||
      !platform::sequence_is_newer(context.sequence, world_.sequence)) {
    return false;
  }
  world_ = state;
  world_.sequence = context.sequence;
  world_.money = bootstrap_.world.money;
  world_.gems = bootstrap_.world.gems;
  world_.skill = bootstrap_.world.skill;
  world_.player_collision = bootstrap_.world.player_collision;
  world_.friendly_fire = bootstrap_.world.friendly_fire;
  return true;
}

bool WorldReplicationState::apply(const GungameState& state, const ApplyContext& context) {
  if (!context.source.from_host || !valid_gungame_state(state) ||
      !platform::sequence_is_newer(context.sequence, gungame_.sequence))
    return false;
  gungame_ = state;
  gungame_.sequence = context.sequence;
  return true;
}

bool WorldReplicationState::apply_bootstrap(const BootstrapState& state, const Sequence sequence) {
  if (!valid_world_state(state.world) ||
      state.synchronized_aid_count > state.synchronized_aids.size() ||
      !finite_vector(state.host_spawn_position) || !finite(state.host_spawn_angle) ||
      !finite(state.host_camera_angle_y) ||
      (sequence != 0 && !platform::sequence_is_newer(sequence, bootstrap_.sequence))) {
    return false;
  }
  bootstrap_ = state;
  bootstrap_.sequence = sequence;
  if (sequence == 0 || platform::sequence_is_newer(sequence, world_.sequence)) {
    world_ = state.world;
    world_.sequence = sequence;
  }
  world_.money = state.world.money;
  world_.gems = state.world.gems;
  world_.skill = state.world.skill;
  world_.player_collision = state.world.player_collision;
  world_.friendly_fire = state.world.friendly_fire;
  return true;
}

void EventReplicationState::reset() {
  sequences_ = {};
  events_.clear();
}

platform::PacketApplyResult EventReplicationState::apply(const GameEventBatch& batch,
                                                         const ApplyContext& context) {
  if (batch.events.empty() || batch.events.size() > UINT8_MAX)
    return platform::PacketApplyResult::REJECT;
  const PlayerId source_player_id = valid_index(context.source.authenticated_player_id, kMaxPlayers)
                                        ? context.source.authenticated_player_id
                                        : batch.events.front().source_player_id;
  if (!valid_index(source_player_id, kMaxPlayers) ||
      !platform::sequence_is_newer(context.sequence, sequences_[source_player_id]) ||
      !std::ranges::all_of(batch.events, [&](const auto& event) {
        return event.source_player_id == source_player_id &&
               source_allows_player(context.source, event.source_player_id) &&
               event.payload_size <= event.payload.size();
      })) {
    return platform::PacketApplyResult::REJECT;
  }
  if (batch.events.size() > kMaximumQueuedEvents - events_.size())
    return platform::PacketApplyResult::CAPACITY_EXCEEDED;
  sequences_[source_player_id] = context.sequence;
  events_.insert(events_.end(), batch.events.begin(), batch.events.end());
  return platform::PacketApplyResult::ACCEPT;
}

void EventReplicationState::depart(const PlayerId player_id) {
  if (!valid_index(player_id, kMaxPlayers))
    return;
  sequences_[player_id] = 0;
  std::erase_if(
      events_, [player_id](const GameEvent& event) { return event.source_player_id == player_id; });
}

std::vector<GameEvent> EventReplicationState::take(const size_t maximum) {
  std::vector<GameEvent> result;
  result.reserve(std::min(maximum, events_.size()));
  while (!events_.empty() && result.size() < maximum) {
    result.push_back(std::move(events_.front()));
    events_.pop_front();
  }
  return result;
}

void ReplicationState::reset() {
  players_.reset();
  traffic_.reset();
  world_.reset();
  entities_.reset();
  events_.reset();
}

void ReplicationState::expire(const uint64_t now_ms) {
  players_.expire(now_ms);
  traffic_.expire(now_ms);
  entities_.expire(now_ms);
}

bool ReplicationState::apply_bootstrap(const BootstrapState& state, const Sequence sequence) {
  return world_.apply_bootstrap(state, sequence);
}

bool ReplicationState::update_player_identity(const PlayerIdentity& identity) {
  return players_.update_identity(identity);
}

bool ReplicationState::depart_player(const PlayerId player_id) {
  if (!valid_index(player_id, kMaxPlayers))
    return false;
  players_.depart(player_id);
  traffic_.clear_source(player_id);
  entities_.depart(player_id);
  events_.depart(player_id);
  return true;
}

}  // namespace multiplayer::jak2::core
