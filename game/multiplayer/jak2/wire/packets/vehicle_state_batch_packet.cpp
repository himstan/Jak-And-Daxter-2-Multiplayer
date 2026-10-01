#include "game/multiplayer/jak2/wire/packets/vehicle_state_batch_packet.h"

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
  return rider_id < kMPMaxPlayers ? rider_id : core::kInvalidPlayerId;
}

}  // namespace

bool validate_packet(const VehicleStateBatchPacket& packet) {
  if (packet.vehicles.size() > core::kMaxVehicles) {
    return false;
  }
  for (const auto& vehicle : packet.vehicles) {
    if (!platform::wire::valid_position_array(vehicle.position) ||
        !platform::wire::valid_quaternion_array(vehicle.quaternion) ||
        !platform::wire::valid_linear_velocity_array(vehicle.linear_velocity) ||
        !platform::wire::valid_angular_velocity_array(vehicle.angular_velocity) ||
        vehicle.state_id > 17u || vehicle.state_flags > 31u) {
      return false;
    }
    for (const uint8_t rider : vehicle.rider_player_ids) {
      if (!valid_wire_rider_reference(rider)) {
        return false;
      }
    }
  }
  return true;
}

VehicleStateBatchPacket to_vehicle_state_batch_packet(const core::TrafficSnapshot& snapshot) {
  VehicleStateBatchPacket packet;
  packet.sample_time_ms = snapshot.sample_time_ms;
  packet.authority_revision = snapshot.authority_revision;
  packet.level_id = snapshot.level_id;
  packet.vehicles.reserve(snapshot.vehicles.size());
  for (const auto& vehicle : snapshot.vehicles) {
    VehicleStateRecord record;
    record.net_id = vehicle.net_id;
    record.vehicle_type = vehicle.vehicle_type;
    record.color_index = vehicle.color_index;
    record.state_id = vehicle.state_id;
    record.position = vehicle.position;
    record.quaternion = vehicle.quaternion;
    record.linear_velocity = vehicle.linear_velocity;
    record.angular_velocity = vehicle.angular_velocity;
    record.state_flags = vehicle.state_flags;
    record.hit_points = vehicle.hit_points;
    record.level_id = vehicle.level_id;
    for (size_t rider = 0; rider < record.rider_player_ids.size(); ++rider) {
      record.rider_player_ids[rider] = to_wire_rider(vehicle.rider_player_ids[rider]);
    }
    packet.vehicles.push_back(record);
  }
  return packet;
}

void from_packet(const VehicleStateBatchPacket& packet, core::TrafficSnapshot& snapshot) {
  snapshot = {};
  snapshot.kind = core::TrafficSnapshot::Kind::VEHICLES;
  snapshot.authority_revision = packet.authority_revision;
  snapshot.level_id = packet.level_id;
  snapshot.sample_time_ms = packet.sample_time_ms;
  snapshot.vehicles.reserve(packet.vehicles.size());
  for (const auto& [net_id, vehicle_type, color_index, state_id, position, quaternion,
                    linear_velocity, angular_velocity, state_flags, hit_points, level_id,
                    rider_player_ids] : packet.vehicles) {
    core::VehicleState vehicle;
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
    for (size_t rider = 0; rider < rider_player_ids.size(); ++rider) {
      vehicle.rider_player_ids[rider] = from_wire_rider(rider_player_ids[rider]);
    }
    vehicle.rider_player_ids[3] = core::kInvalidPlayerId;
    vehicle.target_player_id = vehicle.rider_player_ids[0] < core::kMaxPlayers
                                   ? static_cast<core::PlayerId>(vehicle.rider_player_ids[0])
                                   : core::kInvalidPlayerId;
    snapshot.vehicles.push_back(vehicle);
  }
}

}  // namespace multiplayer::jak2::wire
