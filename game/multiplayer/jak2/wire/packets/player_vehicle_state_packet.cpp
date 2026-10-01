#include "game/multiplayer/jak2/wire/packets/player_vehicle_state_packet.h"

#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"

namespace multiplayer::jak2::wire {
namespace {

uint8_t to_wire_rider(const uint32_t rider_id) {
  if (rider_id == core::kTrafficVehicleCivilianRiderId) {
    return kWireCivilianRiderId;
  }
  return rider_id < kMPMaxPlayers ? static_cast<uint8_t>(rider_id) : kWireInvalidPlayerId;
}

uint32_t from_wire_rider(const uint8_t rider_id) {
  if (rider_id == kWireCivilianRiderId) {
    return core::kTrafficVehicleCivilianRiderId;
  }
  if (rider_id == kWireInvalidPlayerId || rider_id >= kMPMaxPlayers) {
    return core::kInvalidPlayerId;
  }
  return rider_id;
}

}  // namespace

bool validate_packet(const PlayerVehicleStatePacket& packet) {
  const auto& vehicle = packet.vehicle;
  if (vehicle.state_id > 17u || !platform::wire::valid_position_array(vehicle.position) ||
      !platform::wire::valid_quaternion_array(vehicle.quaternion) ||
      !platform::wire::valid_linear_velocity_array(vehicle.linear_velocity) ||
      !platform::wire::valid_angular_velocity_array(vehicle.angular_velocity) ||
      vehicle.state_flags > 31u) {
    return false;
  }
  for (const uint8_t rider : vehicle.rider_player_ids) {
    if (!valid_wire_rider_reference(rider)) {
      return false;
    }
  }
  return true;
}

PlayerVehicleStatePacket to_packet(const core::PlayerVehicleState& state) {
  const auto& source = state.vehicle;
  PlayerVehicleStatePacket packet;
  packet.vehicle.sample_time_ms = source.sample_time_ms;
  packet.vehicle.net_id = source.net_id;
  packet.vehicle.vehicle_type = source.vehicle_type;
  packet.vehicle.color_index = source.color_index;
  packet.vehicle.state_id = source.state_id;
  packet.vehicle.position = source.position;
  packet.vehicle.quaternion = source.quaternion;
  packet.vehicle.linear_velocity = source.linear_velocity;
  packet.vehicle.angular_velocity = source.angular_velocity;
  packet.vehicle.state_flags = source.state_flags;
  packet.vehicle.hit_points = source.hit_points;
  packet.vehicle.level_id = source.level_id;
  for (size_t index = 0; index < source.rider_player_ids.size(); ++index) {
    packet.vehicle.rider_player_ids[index] = to_wire_rider(source.rider_player_ids[index]);
  }
  return packet;
}

void from_packet(const PlayerVehicleStatePacket& packet, core::PlayerVehicleState& state) {
  state = {};
  const auto& [sample_time_ms, net_id, vehicle_type, color_index, state_id, position, quaternion,
               linear_velocity, angular_velocity, state_flags, hit_points, level_id,
               rider_player_ids] = packet.vehicle;
  auto& vehicle = state.vehicle;
  vehicle.sample_time_ms = sample_time_ms;
  vehicle.net_id = net_id;
  vehicle.vehicle_type = vehicle_type;
  vehicle.color_index = color_index;
  vehicle.state_id = state_id;
  vehicle.position = position;
  vehicle.quaternion = quaternion;
  vehicle.linear_velocity = linear_velocity;
  vehicle.angular_velocity = angular_velocity;
  vehicle.state_flags = state_flags;
  vehicle.hit_points = hit_points;
  vehicle.level_id = level_id;
  for (size_t index = 0; index < vehicle.rider_player_ids.size(); ++index) {
    vehicle.rider_player_ids[index] = from_wire_rider(rider_player_ids[index]);
  }
  vehicle.target_player_id = vehicle.rider_player_ids[0] < core::kMaxPlayers
                                 ? static_cast<core::PlayerId>(vehicle.rider_player_ids[0])
                                 : core::kInvalidPlayerId;
}

}  // namespace multiplayer::jak2::wire
