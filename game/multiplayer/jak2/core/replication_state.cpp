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
  vehicle_release_deadlines_ = {};
  turrets_ = {};
  turret_sequences_ = {};
}

void PlayerReplicationState::expire(const uint64_t now_ms) {
  for (PlayerSlot player_slot = 0; player_slot < kMaxPlayers; ++player_slot) {
    auto& player = player_states_[player_slot];
    if (vehicle_release_deadlines_[player_slot] != 0 &&
        now_ms >= vehicle_release_deadlines_[player_slot]) {
      player_vehicles_[player_slot] = {};
      vehicle_release_deadlines_[player_slot] = 0;
    }
    if (!player.state_ready || now_ms - player.received_time_ms <= 2000)
      continue;
    lg::debug("[MP-PlayerState] Expiring player {}: age={}ms sequence={} activity={} vehicle={}.",
              player_slot, now_ms - player.received_time_ms, player.last_sequence,
              static_cast<uint8_t>(player.activity), player.vehicle_id);
    player.state_ready = false;
    identities_[player_slot].state_ready = false;
    player_vehicles_[player_slot] = {};
    player_vehicle_sequences_[player_slot] = 0;
    vehicle_release_deadlines_[player_slot] = 0;
    turrets_[player_slot] = {};
    turret_sequences_[player_slot] = 0;
  }
}

bool PlayerReplicationState::apply(const PlayerState& state, const ApplyContext& context) {
  if (!valid_index(state.player_slot, kMaxPlayers) ||
      !source_allows_player(context.source, state.player_slot) || !finite_vector(state.position) ||
      !finite_vector(state.velocity) || !finite(state.angle) || !finite(state.camera_angle_y) ||
      !valid_player_level_state(state.levels, state.primary_level_slot) ||
      !platform::sequence_is_newer(context.sequence,
                                   player_states_[state.player_slot].last_sequence)) {
    return false;
  }
  auto& current = player_states_[state.player_slot];
  if (current.vehicle_id != state.vehicle_id) {
    const bool releasing_driver =
        state.vehicle_id == 0 && current.vehicle_id != 0 && current.vehicle_seat == 0 &&
        !state.spectator_only && state.selected_traffic_authority < kMaxPlayers &&
        state.selected_traffic_authority != state.player_slot &&
        (current.vehicle_id & kTrafficNetIdClassMask) != kFixedTrafficNetIdClass &&
        player_vehicles_[state.player_slot].vehicle.net_id == current.vehicle_id &&
        std::ranges::none_of(player_states_, [&](const auto& player) {
          return player.player_slot != state.player_slot &&
                 player.vehicle_id == current.vehicle_id && player.vehicle_seat == 0;
        });
    vehicle_release_deadlines_[state.player_slot] =
        releasing_driver ? context.received_at_ms + 2000 : 0;
    if (!releasing_driver) {
      player_vehicles_[state.player_slot] = {};
      player_vehicle_sequences_[state.player_slot] = 0;
    }
    turrets_[state.player_slot] = {};
    turret_sequences_[state.player_slot] = 0;
  }
  current = state;
  current.last_sequence = context.sequence;
  current.received_time_ms = context.received_at_ms;
  if (state.spectator_only || (vehicle_release_deadlines_[state.player_slot] != 0 &&
                               state.selected_traffic_authority == state.player_slot)) {
    vehicle_release_deadlines_[state.player_slot] = 0;
    player_vehicles_[state.player_slot] = {};
  }
  if (state.vehicle_id != 0 && state.vehicle_seat == 0) {
    for (PlayerSlot other = 0; other < kMaxPlayers; ++other) {
      if (other != state.player_slot && vehicle_release_deadlines_[other] != 0 &&
          player_vehicles_[other].vehicle.net_id == state.vehicle_id) {
        vehicle_release_deadlines_[other] = 0;
        player_vehicles_[other] = {};
      }
    }
  }
  player_vehicles_[state.player_slot].seat_index = state.vehicle_seat;
  if (identities_[state.player_slot].joined) {
    identities_[state.player_slot].state_ready = state.state_ready;
    identities_[state.player_slot].spectator_only = state.spectator_only;
  }
  if (!state.turret_active) {
    turrets_[state.player_slot] = {};
    turret_sequences_[state.player_slot] = 0;
  }
  return true;
}

bool PlayerReplicationState::apply(const PlayerVehicleState& state, const ApplyContext& context) {
  if (!valid_index(state.player_slot, kMaxPlayers) ||
      !source_allows_player(context.source, state.player_slot) ||
      !valid_player_vehicle_state(state.vehicle) ||
      (player_states_[state.player_slot].last_sequence != 0 &&
       !owns_vehicle(state.player_slot, state.vehicle.net_id, context.received_at_ms)) ||
      !platform::sequence_is_newer(context.sequence,
                                   player_vehicle_sequences_[state.player_slot])) {
    return false;
  }
  auto& player = player_states_[state.player_slot];
  if (player.last_sequence == 0) {
    player.vehicle_id = state.vehicle.net_id;
    player.vehicle_seat = state.seat_index;
  }
  player_vehicle_sequences_[state.player_slot] = context.sequence;
  player_vehicles_[state.player_slot] = state;
  player_vehicles_[state.player_slot].seat_index = player.vehicle_seat;
  player_vehicles_[state.player_slot].vehicle.last_sequence = context.sequence;
  player_vehicles_[state.player_slot].vehicle.received_time_ms = context.received_at_ms;
  return true;
}

bool PlayerReplicationState::owns_vehicle(const PlayerSlot player_slot,
                                          const EntityId net_id,
                                          const uint64_t now_ms) const {
  if (!valid_index(player_slot, kMaxPlayers) || net_id == 0)
    return false;
  return player_states_[player_slot].vehicle_id == net_id ||
         (vehicle_release_deadlines_[player_slot] > now_ms &&
          player_vehicles_[player_slot].vehicle.net_id == net_id);
}

bool PlayerReplicationState::apply(const TurretState& state, const ApplyContext& context) {
  if (!valid_index(state.player_slot, kMaxPlayers) ||
      !source_allows_player(context.source, state.player_slot) || !finite(state.rotation_x) ||
      !finite(state.rotation_y) || state.turret_aid == 0 ||
      (player_states_[state.player_slot].last_sequence != 0 &&
       (!player_states_[state.player_slot].turret_active ||
        player_states_[state.player_slot].vehicle_id != state.turret_aid)) ||
      !platform::sequence_is_newer(context.sequence, turret_sequences_[state.player_slot])) {
    return false;
  }
  turrets_[state.player_slot] = state;
  turret_sequences_[state.player_slot] = context.sequence;
  player_states_[state.player_slot].turret_active = true;
  return true;
}

bool PlayerReplicationState::update_identity(const PlayerIdentity& identity) {
  if (!valid_index(identity.player_slot, kMaxPlayers) || !valid_character(identity.character) ||
      !valid_name(identity.name) || !is_player_skin_valid(identity.skin)) {
    return false;
  }
  identities_[identity.player_slot] = identity;
  identities_[identity.player_slot].identity_ready = true;
  identities_[identity.player_slot].joined = true;
  return true;
}

void PlayerReplicationState::depart(const PlayerSlot player_slot) {
  if (!valid_index(player_slot, kMaxPlayers))
    return;
  identities_[player_slot] = {};
  player_states_[player_slot] = {};
  player_vehicles_[player_slot] = {};
  player_vehicle_sequences_[player_slot] = 0;
  vehicle_release_deadlines_[player_slot] = 0;
  turrets_[player_slot] = {};
  turret_sequences_[player_slot] = 0;
}

void WorldReplicationState::reset() {
  world_ = {};
  gungame_ = {};
  bootstrap_ = {};
  player_rules_sequence_ = 0;
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
  world_.respawn_delay_seconds = bootstrap_.world.respawn_delay_seconds;
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

bool WorldReplicationState::apply(const PlayerRulesState& state, const ApplyContext& context) {
  if (!context.source.from_host ||
      !platform::sequence_is_newer(context.sequence, player_rules_sequence_))
    return false;
  player_rules_sequence_ = context.sequence;
  world_.respawn_delay_seconds = bootstrap_.world.respawn_delay_seconds =
      state.respawn_delay_seconds;
  world_.player_collision = bootstrap_.world.player_collision = state.player_collision;
  world_.friendly_fire = bootstrap_.world.friendly_fire = state.friendly_fire;
  return true;
}

bool WorldReplicationState::apply_bootstrap(const BootstrapState& state, const Sequence sequence) {
  if ((state.player_data && !valid_player_data(*state.player_data)) ||
      !valid_world_state(state.world) ||
      state.synchronized_aid_count > state.synchronized_aids.size() ||
      !finite_vector(state.host_spawn_position) || !finite(state.host_spawn_angle) ||
      !finite(state.host_camera_angle_y) ||
      (sequence != 0 && !platform::sequence_is_newer(sequence, bootstrap_.sequence))) {
    return false;
  }
  const PlayerRulesState live_rules = {.respawn_delay_seconds = world_.respawn_delay_seconds,
                                       .player_collision = world_.player_collision,
                                       .friendly_fire = world_.friendly_fire};
  bootstrap_ = state;
  bootstrap_.sequence = sequence;
  if (sequence != 0 && player_rules_sequence_ != 0) {
    bootstrap_.world.respawn_delay_seconds = live_rules.respawn_delay_seconds;
    bootstrap_.world.player_collision = live_rules.player_collision;
    bootstrap_.world.friendly_fire = live_rules.friendly_fire;
  }
  if (sequence == 0 || platform::sequence_is_newer(sequence, world_.sequence)) {
    world_ = state.world;
    world_.sequence = sequence;
  }
  world_.money = state.world.money;
  world_.gems = state.world.gems;
  world_.skill = state.world.skill;
  world_.respawn_delay_seconds = bootstrap_.world.respawn_delay_seconds;
  world_.player_collision = bootstrap_.world.player_collision;
  world_.friendly_fire = bootstrap_.world.friendly_fire;
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
  const PlayerSlot source_player_slot =
      valid_index(context.source.authenticated_player_slot, kMaxPlayers)
          ? context.source.authenticated_player_slot
          : batch.events.front().source_player_slot;
  if (!valid_index(source_player_slot, kMaxPlayers) ||
      !platform::sequence_is_newer(context.sequence, sequences_[source_player_slot]) ||
      !std::ranges::all_of(batch.events, [&](const auto& event) {
        return event.source_player_slot == source_player_slot &&
               source_allows_player(context.source, event.source_player_slot) &&
               event.payload_size <= event.payload.size();
      })) {
    return platform::PacketApplyResult::REJECT;
  }
  if (batch.events.size() > kMaximumQueuedEvents - events_.size())
    return platform::PacketApplyResult::CAPACITY_EXCEEDED;
  sequences_[source_player_slot] = context.sequence;
  events_.insert(events_.end(), batch.events.begin(), batch.events.end());
  return platform::PacketApplyResult::ACCEPT;
}

void EventReplicationState::depart(const PlayerSlot player_slot) {
  if (!valid_index(player_slot, kMaxPlayers))
    return;
  sequences_[player_slot] = 0;
  std::erase_if(events_, [player_slot](const GameEvent& event) {
    return event.source_player_slot == player_slot;
  });
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
  traffic_.expire(now_ms, players_.player_vehicles());
  players_.expire(now_ms);
  entities_.expire(now_ms);
}

bool ReplicationState::apply_bootstrap(const BootstrapState& state, const Sequence sequence) {
  return world_.apply_bootstrap(state, sequence);
}

bool ReplicationState::update_player_identity(const PlayerIdentity& identity) {
  return players_.update_identity(identity);
}

bool ReplicationState::depart_player(const PlayerSlot player_slot) {
  if (!valid_index(player_slot, kMaxPlayers))
    return false;
  players_.depart(player_slot);
  traffic_.clear_source(player_slot);
  entities_.depart(player_slot);
  events_.depart(player_slot);
  return true;
}

}  // namespace multiplayer::jak2::core
