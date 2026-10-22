#include <algorithm>

#include "game/multiplayer/jak2/core/replication_state.h"
#include "game/multiplayer/jak2/core/validation.h"
#include "game/multiplayer/platform/core/sequence.h"

namespace multiplayer::jak2::core {
namespace {

template <typename State>
bool expire_entities(std::vector<State>& entities, const uint64_t now_ms) {
  return std::erase_if(entities, [now_ms](const auto& entity) {
           return now_ms - entity.received_time_ms > 2000;
         }) != 0;
}

template <typename State>
void merge_entity(std::vector<State>& destination, const State& incoming, const Sequence sequence) {
  const auto existing = std::ranges::find_if(
      destination, [&](const State& state) { return state.net_id == incoming.net_id; });
  if (existing == destination.end()) {
    auto state = incoming;
    state.last_sequence = sequence;
    destination.push_back(std::move(state));
  } else if (platform::sequence_is_current_or_newer(sequence, existing->last_sequence)) {
    *existing = incoming;
    existing->last_sequence = sequence;
  }
}

}  // namespace

void TrafficReplicationState::reset() {
  authority_ = {};
  authority_.assignments.fill(kInvalidPlayerSlot);
  selected_authority_ = kInvalidPlayerSlot;
  clear();
}

void TrafficReplicationState::expire(const uint64_t now_ms,
                                     const std::span<const PlayerVehicleState> player_vehicles) {
  bool changed = false;
  for (PlayerSlot source = 0; source < kMaxPlayers; ++source) {
    changed |= expire_entities(pedestrian_snapshots_[source].pedestrians, now_ms);
    changed |= std::erase_if(vehicle_snapshots_[source].vehicles, [&](const auto& vehicle) {
                 return now_ms - vehicle.received_time_ms > 2000 ||
                        std::ranges::any_of(player_vehicles, [&](const auto& player_vehicle) {
                          return player_vehicle.seat_index == 0 &&
                                 player_vehicle.vehicle.net_id == vehicle.net_id;
                        });
               }) != 0;
  }
  if (changed) {
    rebuild_aggregate();
    rebuild_selected();
  }
}

bool TrafficReplicationState::apply(const TrafficAuthority& authority,
                                    const ApplyContext& context) {
  if (!context.source.from_host || !valid_traffic_authority(authority))
    return false;
  if (authority.revision == authority_.revision)
    return authority.assignments == authority_.assignments;
  if (!platform::sequence_is_newer(authority.revision, authority_.revision))
    return false;
  const bool selection_valid = valid_index(selected_authority_, kMaxPlayers) &&
                               authority.assignments[selected_authority_] == selected_authority_;
  const bool local_assignment_changed = valid_index(context.local_player_slot, kMaxPlayers) &&
                                        authority.assignments[context.local_player_slot] !=
                                            authority_.assignments[context.local_player_slot];
  authority_ = authority;
  if ((!selection_valid || local_assignment_changed) &&
      valid_index(context.local_player_slot, kMaxPlayers)) {
    selected_authority_ = authority_.assignments[context.local_player_slot];
  } else if (!selection_valid) {
    selected_authority_ = kInvalidPlayerSlot;
  }
  clear();
  return true;
}

bool TrafficReplicationState::apply(const TrafficSnapshot& incoming, const ApplyContext& context) {
  if ((incoming.kind != TrafficSnapshot::Kind::PEDESTRIANS &&
       incoming.kind != TrafficSnapshot::Kind::VEHICLES) ||
      incoming.authority_revision == 0 || incoming.authority_revision != authority_.revision) {
    return false;
  }
  const auto source_player_slot = snapshot_source(incoming.source_player_slot, context.source);
  if (!valid_index(source_player_slot, kMaxPlayers) ||
      (!context.source.from_host &&
       context.source.authenticated_player_slot != kInvalidPlayerSlot &&
       source_player_slot != context.source.authenticated_player_slot) ||
      authority_.assignments[source_player_slot] != source_player_slot) {
    return false;
  }
  if (incoming.kind == TrafficSnapshot::Kind::PEDESTRIANS) {
    if (incoming.pedestrians.size() > kMaxPedestrians || !incoming.vehicles.empty() ||
        !std::ranges::all_of(incoming.pedestrians,
                             [&](const auto& pedestrian) {
                               return valid_pedestrian_state(pedestrian) &&
                                      valid_traffic_pedestrian_net_id(pedestrian.net_id,
                                                                      source_player_slot) &&
                                      (pedestrian.vehicle_net_id == 0 ||
                                       valid_traffic_vehicle_net_id(pedestrian.vehicle_net_id));
                             }) ||
        !platform::sequence_is_newer(context.sequence, pedestrian_sequences_[source_player_slot])) {
      return false;
    }
  } else if (incoming.vehicles.size() > kMaxVehicles || !incoming.pedestrians.empty() ||
             !std::ranges::all_of(incoming.vehicles, valid_vehicle_state) ||
             !platform::sequence_is_newer(context.sequence,
                                          vehicle_sequences_[source_player_slot])) {
    return false;
  }
  prepare_level(source_player_slot, incoming.level_id);
  if (incoming.kind == TrafficSnapshot::Kind::PEDESTRIANS) {
    auto& snapshot = pedestrian_snapshots_[source_player_slot];
    snapshot = incoming;
    for (auto& pedestrian : snapshot.pedestrians) {
      pedestrian.sample_time_ms = incoming.sample_time_ms;
      pedestrian.last_sequence = context.sequence;
      pedestrian.received_time_ms = context.received_at_ms;
    }
    pedestrian_sequences_[source_player_slot] = context.sequence;
    snapshot.source_player_slot = source_player_slot;
    snapshot.sequence = context.sequence;
  } else {
    auto& snapshot = vehicle_snapshots_[source_player_slot];
    snapshot = incoming;
    for (auto& vehicle : snapshot.vehicles) {
      vehicle.sample_time_ms = incoming.sample_time_ms;
      vehicle.last_sequence = context.sequence;
      vehicle.received_time_ms = context.received_at_ms;
    }
    vehicle_sequences_[source_player_slot] = context.sequence;
    snapshot.source_player_slot = source_player_slot;
    snapshot.sequence = context.sequence;
  }
  rebuild_aggregate();
  rebuild_selected();
  return true;
}

bool TrafficReplicationState::select_authority(const PlayerSlot source_player_slot) {
  const PlayerSlot normalized =
      valid_index(source_player_slot, kMaxPlayers) &&
              authority_.assignments[source_player_slot] == source_player_slot
          ? source_player_slot
          : kInvalidPlayerSlot;
  if (normalized == selected_authority_)
    return true;
  selected_authority_ = normalized;
  clear();
  return true;
}

void TrafficReplicationState::clear() {
  pedestrian_sequences_ = {};
  vehicle_sequences_ = {};
  pedestrian_snapshots_ = {};
  vehicle_snapshots_ = {};
  aggregate_ = {};
  selected_ = {};
}

void TrafficReplicationState::clear_source(const PlayerSlot source_player_slot) {
  if (!valid_index(source_player_slot, kMaxPlayers))
    return;
  pedestrian_sequences_[source_player_slot] = 0;
  vehicle_sequences_[source_player_slot] = 0;
  pedestrian_snapshots_[source_player_slot] = {};
  vehicle_snapshots_[source_player_slot] = {};
  rebuild_aggregate();
  rebuild_selected();
}

void TrafficReplicationState::clear_source(const PlayerSlot source_player_slot,
                                           const TrafficSnapshot::Kind kind) {
  if (!valid_index(source_player_slot, kMaxPlayers))
    return;
  if (kind == TrafficSnapshot::Kind::PEDESTRIANS) {
    pedestrian_sequences_[source_player_slot] = 0;
    pedestrian_snapshots_[source_player_slot] = {};
  } else {
    vehicle_sequences_[source_player_slot] = 0;
    vehicle_snapshots_[source_player_slot] = {};
  }
  rebuild_aggregate();
  rebuild_selected();
}

PlayerSlot TrafficReplicationState::vehicle_source(const EntityId net_id) const {
  for (PlayerSlot source = 0; source < kMaxPlayers; ++source) {
    if (std::ranges::any_of(vehicle_snapshots_[source].vehicles,
                            [&](const auto& vehicle) { return vehicle.net_id == net_id; })) {
      return source;
    }
  }
  return kInvalidPlayerSlot;
}

void TrafficReplicationState::prepare_level(const PlayerSlot source_player_slot,
                                            const uint32_t level_id) {
  auto& pedestrians = pedestrian_snapshots_[source_player_slot];
  auto& vehicles = vehicle_snapshots_[source_player_slot];
  if (level_id == 0)
    return;
  if ((pedestrians.level_id != 0 && pedestrians.level_id != level_id) ||
      (vehicles.level_id != 0 && vehicles.level_id != level_id)) {
    pedestrians.pedestrians.clear();
    vehicles.vehicles.clear();
  }
  pedestrians.level_id = vehicles.level_id = level_id;
}

void TrafficReplicationState::rebuild_aggregate() {
  aggregate_ = {};
  aggregate_.kind = TrafficSnapshot::Kind::VEHICLES;
  for (PlayerSlot source = 0; source < kMaxPlayers; ++source) {
    const auto& pedestrians = pedestrian_snapshots_[source];
    for (const auto& pedestrian : pedestrians.pedestrians)
      merge_entity(aggregate_.pedestrians, pedestrian, pedestrians.sequence);
    const auto& vehicles = vehicle_snapshots_[source];
    for (const auto& vehicle : vehicles.vehicles)
      merge_entity(aggregate_.vehicles, vehicle, vehicles.sequence);
    const auto update_metadata = [&](const TrafficSnapshot& snapshot) {
      aggregate_.source_player_slot = snapshot.source_player_slot;
      aggregate_.authority_revision = snapshot.authority_revision;
      aggregate_.level_id = snapshot.level_id;
      aggregate_.sample_time_ms = snapshot.sample_time_ms;
      aggregate_.sequence = snapshot.sequence;
      aggregate_.kind = snapshot.kind;
    };
    if (pedestrians.sequence >= aggregate_.sequence)
      update_metadata(pedestrians);
    if (vehicles.sequence >= aggregate_.sequence)
      update_metadata(vehicles);
  }
}

void TrafficReplicationState::rebuild_selected() {
  selected_ = {};
  selected_.kind = TrafficSnapshot::Kind::VEHICLES;
  if (!valid_index(selected_authority_, kMaxPlayers))
    return;
  const auto& pedestrians = pedestrian_snapshots_[selected_authority_];
  const auto& vehicles = vehicle_snapshots_[selected_authority_];
  selected_.pedestrians = pedestrians.pedestrians;
  selected_.vehicles = vehicles.vehicles;
  const auto& latest = pedestrians.sequence >= vehicles.sequence ? pedestrians : vehicles;
  selected_.source_player_slot = latest.source_player_slot;
  selected_.authority_revision = latest.authority_revision;
  selected_.level_id = latest.level_id;
  selected_.sample_time_ms = latest.sample_time_ms;
  selected_.sequence = latest.sequence;
  selected_.kind = latest.kind;
}

}  // namespace multiplayer::jak2::core
