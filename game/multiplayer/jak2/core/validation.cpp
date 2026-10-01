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

bool source_allows_player(const platform::MessageOrigin& source, const PlayerId player_id) {
  return source.from_host || source.authenticated_player_id == kInvalidPlayerId ||
         source.authenticated_player_id == player_id;
}

PlayerId snapshot_source(const PlayerId snapshot_source_id, const platform::MessageOrigin& source) {
  return valid_index(snapshot_source_id, kMaxPlayers) ? snapshot_source_id
                                                      : source.authenticated_player_id;
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
         (enemy.owner_player_id == kInvalidPlayerId ||
          valid_index(enemy.owner_player_id, kMaxPlayers)) &&
         (enemy.focus_player_id == kInvalidPlayerId ||
          valid_index(enemy.focus_player_id, kMaxPlayers)) &&
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

bool valid_traffic_origin(const PlayerId player_id) {
  return valid_index(player_id, kMaxPlayers);
}

bool valid_ambient_sequence(const uint32_t sequence, const uint32_t first_sequence) {
  return sequence >= first_sequence && sequence <= kTrafficNetIdSequenceMask;
}

uint32_t traffic_net_id_class(const uint32_t net_id) {
  return net_id & kTrafficNetIdClassMask;
}

PlayerId traffic_net_id_origin(const uint32_t net_id) {
  return static_cast<PlayerId>((net_id & kTrafficNetIdOriginMask) >> kTrafficNetIdOriginShift);
}

uint32_t traffic_net_id_sequence(const uint32_t net_id) {
  return net_id & kTrafficNetIdSequenceMask;
}

}  // namespace

bool valid_traffic_pedestrian_net_id(const uint32_t net_id, const PlayerId source_player_id) {
  const uint32_t entity_class = traffic_net_id_class(net_id);
  if (entity_class == kFixedTrafficNetIdClass) {
    return (net_id & kFixedTrafficNetIdNamespaceMask) == kShuttlePedestrianNetIdNamespace;
  }
  return entity_class == kTrafficPedestrianNetIdClass && valid_traffic_origin(source_player_id) &&
         traffic_net_id_origin(net_id) == source_player_id &&
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
  if (vehicle.target_player_id != kInvalidPlayerId &&
      !valid_index(vehicle.target_player_id, kMaxPlayers)) {
    return false;
  }
  return std::ranges::all_of(vehicle.rider_player_ids, [](const uint32_t player_id) {
    return player_id == kInvalidPlayerId || player_id == kTrafficVehicleCivilianRiderId ||
           valid_index(player_id, kMaxPlayers);
  });
}

std::array<PlayerId, kMaxPlayers> normalize_traffic_authority(
    const std::array<PlayerId, kMaxPlayers>& assignments) {
  std::array<PlayerId, kMaxPlayers> normalized = {};
  normalized.fill(kInvalidPlayerId);
  for (PlayerId player_id = 0; player_id < kMaxPlayers; ++player_id) {
    if (valid_index(assignments[player_id], kMaxPlayers)) {
      normalized[player_id] = assignments[player_id];
    }
  }
  for (PlayerId player_id = 0; player_id < kMaxPlayers; ++player_id) {
    if (const PlayerId source = normalized[player_id];
        source != kInvalidPlayerId && normalized[source] != source) {
      normalized[player_id] = kInvalidPlayerId;
    }
  }
  return normalized;
}

bool valid_traffic_authority(const TrafficAuthority& authority) {
  return authority.revision != 0 &&
         normalize_traffic_authority(authority.assignments) == authority.assignments;
}

}  // namespace multiplayer::jak2::core
