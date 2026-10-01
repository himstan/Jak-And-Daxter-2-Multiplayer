#pragma once

#include <array>
#include <cstdint>

#include "game/multiplayer/jak2/core/multiplayer_types.h"

namespace multiplayer::jak2::core {

bool finite(float value);
bool finite_vector(const std::array<float, 3>& values);
bool finite_quaternion(const std::array<float, 4>& values);
bool count_fits(uint32_t count, uint32_t maximum);
bool valid_index(uint32_t index, uint32_t limit);
bool source_allows_player(const platform::MessageOrigin& source, PlayerId player_id);
PlayerId snapshot_source(PlayerId snapshot_source_id, const platform::MessageOrigin& source);
bool valid_world_state(const WorldState& world);
bool valid_vehicle_state(const VehicleState& vehicle);
bool valid_player_vehicle_state(const VehicleState& vehicle);
bool valid_pedestrian_state(const PedestrianState& pedestrian);
bool valid_enemy_state(const EnemyState& enemy);
bool valid_boss_state(const BossState& boss);
bool valid_traffic_pedestrian_net_id(uint32_t net_id, PlayerId source_player_id);
bool valid_traffic_vehicle_net_id(uint32_t net_id);
bool valid_traffic_vehicle_occupants(const VehicleState& vehicle);
bool valid_traffic_authority(const TrafficAuthority& authority);
std::array<PlayerId, kMaxPlayers> normalize_traffic_authority(
    const std::array<PlayerId, kMaxPlayers>& assignments);

}  // namespace multiplayer::jak2::core
