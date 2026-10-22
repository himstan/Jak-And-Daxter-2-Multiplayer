#include "validation.h"

#include <algorithm>

namespace multiplayer::jak2::core {

bool finite(const float value) {
  return std::isfinite(value);
}

bool finite_vector(const std::array<float, 3>& values) {
  return std::ranges::all_of(values, finite);
}

bool finite_quaternion(const std::array<float, 4>& values) {
  return std::ranges::all_of(values, finite);
}

bool count_fits(const uint32_t count, const uint32_t maximum) {
  return count <= maximum;
}

bool valid_index(const uint32_t index, const uint32_t limit) {
  return index < limit;
}

bool source_allows_player(const platform::MessageOrigin& source, const PlayerSlot player_slot) {
  return source.from_host || source.authenticated_player_slot == kInvalidPlayerSlot ||
         source.authenticated_player_slot == player_slot;
}

PlayerSlot snapshot_source(const PlayerSlot snapshot_source_id,
                           const platform::MessageOrigin& source) {
  return valid_index(snapshot_source_id, kMaxPlayers) ? snapshot_source_id
                                                      : source.authenticated_player_slot;
}

bool valid_gungame_state(const GungameState& state) {
  if (state.targets.size() > UINT16_MAX || state.course_id > 4 || state.end_door > 1 ||
      state.red_intro_step > 4 || state.yellow_intro_step > 3 ||
      state.phase > GungamePhase::FINISHED || !std::ranges::all_of(state.targets, [](auto target) {
        return target.state <= GungameTargetState::BROKEN && target.spawn_time >= 0 &&
               (target.state != GungameTargetState::NOT_SPAWNED || target.spawn_time == 0);
      }))
    return false;
  if (state.phase == GungamePhase::INACTIVE)
    return state.course_id == 0 && state.targets.empty() && !state.open_end;
  if (state.run_id == 0 || state.course_id == 0)
    return false;
  if (state.phase == GungamePhase::RED_INTRO || state.phase == GungamePhase::YELLOW_INTRO)
    return state.course_id == (state.phase == GungamePhase::RED_INTRO ? 1 : 2) &&
           state.targets.empty() && !state.open_end;
  return !state.targets.empty() && (state.phase != GungamePhase::FINISHED || !state.open_end);
}

bool valid_world_state(const WorldState& world) {
  return finite(world.money) && finite(world.gems) && finite(world.skill) &&
         finite(world.time_of_day_ratio) && finite(world.weather_cloud) &&
         finite(world.weather_fog) && finite(world.weather_rain);
}

bool valid_vehicle_state(const VehicleState& vehicle) {
  return vehicle.net_id != 0 && valid_traffic_vehicle_net_id(vehicle.net_id) &&
         valid_traffic_vehicle_occupants(vehicle) && finite_vector(vehicle.position) &&
         finite_quaternion(vehicle.quaternion) && finite_vector(vehicle.linear_velocity) &&
         finite_vector(vehicle.angular_velocity);
}

bool valid_player_vehicle_state(const VehicleState& vehicle) {
  return vehicle.net_id != 0 && valid_traffic_vehicle_occupants(vehicle) &&
         finite_vector(vehicle.position) && finite_quaternion(vehicle.quaternion) &&
         finite_vector(vehicle.linear_velocity) && finite_vector(vehicle.angular_velocity);
}

bool valid_pedestrian_state(const PedestrianState& pedestrian) {
  return pedestrian.net_id != 0 && finite_vector(pedestrian.position) &&
         finite_quaternion(pedestrian.quaternion);
}

bool valid_enemy_state(const EnemyState& enemy) {
  return enemy.actor_id != 0 &&
         (enemy.owner_player_slot == kInvalidPlayerSlot ||
          valid_index(enemy.owner_player_slot, kMaxPlayers)) &&
         (enemy.focus_player_slot == kInvalidPlayerSlot ||
          valid_index(enemy.focus_player_slot, kMaxPlayers)) &&
         finite_vector(enemy.position) && finite_quaternion(enemy.quaternion);
}

bool valid_boss_state(const BossState& boss) {
  return boss.active <= 1 &&
         boss.state_id <= (boss.kind == BossState::Kind::PALACE_SQUID ? 15u : 25u) &&
         finite(boss.shield_hit_points) && finite_vector(boss.position) &&
         finite_quaternion(boss.quaternion) && finite_vector(boss.root_position) &&
         finite_quaternion(boss.root_quaternion) && finite_vector(boss.trajectory_source) &&
         finite_vector(boss.trajectory_destination) && finite(boss.trajectory_duration);
}

namespace {

bool valid_traffic_origin(const PlayerSlot player_slot) {
  return valid_index(player_slot, kMaxPlayers);
}

bool valid_ambient_sequence(const uint32_t sequence, const uint32_t first_sequence) {
  return sequence >= first_sequence && sequence <= kTrafficNetIdSequenceMask;
}

uint32_t traffic_net_id_class(const uint32_t net_id) {
  return net_id & kTrafficNetIdClassMask;
}

PlayerSlot traffic_net_id_origin(const uint32_t net_id) {
  return static_cast<PlayerSlot>((net_id & kTrafficNetIdOriginMask) >> kTrafficNetIdOriginShift);
}

uint32_t traffic_net_id_sequence(const uint32_t net_id) {
  return net_id & kTrafficNetIdSequenceMask;
}

}  // namespace

bool valid_traffic_pedestrian_net_id(const uint32_t net_id, const PlayerSlot source_player_slot) {
  const uint32_t entity_class = traffic_net_id_class(net_id);
  if (entity_class == kFixedTrafficNetIdClass) {
    return (net_id & kFixedTrafficNetIdNamespaceMask) == kShuttlePedestrianNetIdNamespace;
  }
  return entity_class == kTrafficPedestrianNetIdClass && valid_traffic_origin(source_player_slot) &&
         traffic_net_id_origin(net_id) == source_player_slot &&
         valid_ambient_sequence(traffic_net_id_sequence(net_id), 1u);
}

bool valid_traffic_vehicle_net_id(const uint32_t net_id) {
  const uint32_t entity_class = traffic_net_id_class(net_id);
  if (entity_class == kFixedTrafficNetIdClass) {
    return (net_id & kFixedTrafficNetIdNamespaceMask) == kMissionVehicleNetIdNamespace &&
           net_id != kMissionVehicleNetIdNamespace;
  }
  if (!valid_traffic_origin(traffic_net_id_origin(net_id))) {
    return false;
  }
  if (entity_class == kTrafficVehicleNetIdClass) {
    return valid_ambient_sequence(traffic_net_id_sequence(net_id), 1u);
  }
  return entity_class == kPlayerVehicleNetIdClass;
}

bool valid_traffic_vehicle_occupants(const VehicleState& vehicle) {
  if (vehicle.target_player_slot != kInvalidPlayerSlot &&
      !valid_index(vehicle.target_player_slot, kMaxPlayers)) {
    return false;
  }
  return std::ranges::all_of(vehicle.rider_player_slots, [](const uint32_t player_slot) {
    return player_slot == kInvalidPlayerSlot || player_slot == kTrafficVehicleCivilianRiderId ||
           valid_index(player_slot, kMaxPlayers);
  });
}

std::array<PlayerSlot, kMaxPlayers> normalize_traffic_authority(
    const std::array<PlayerSlot, kMaxPlayers>& assignments) {
  std::array<PlayerSlot, kMaxPlayers> normalized = {};
  normalized.fill(kInvalidPlayerSlot);
  for (PlayerSlot player_slot = 0; player_slot < kMaxPlayers; ++player_slot) {
    if (valid_index(assignments[player_slot], kMaxPlayers)) {
      normalized[player_slot] = assignments[player_slot];
    }
  }
  for (PlayerSlot player_slot = 0; player_slot < kMaxPlayers; ++player_slot) {
    if (const PlayerSlot source = normalized[player_slot];
        source != kInvalidPlayerSlot && normalized[source] != source) {
      normalized[player_slot] = kInvalidPlayerSlot;
    }
  }
  return normalized;
}

bool valid_traffic_authority(const TrafficAuthority& authority) {
  return authority.revision != 0 &&
         normalize_traffic_authority(authority.assignments) == authority.assignments;
}

}  // namespace multiplayer::jak2::core
