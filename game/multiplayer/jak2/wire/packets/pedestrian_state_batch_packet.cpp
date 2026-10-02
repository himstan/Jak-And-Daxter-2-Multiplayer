#include "game/multiplayer/jak2/wire/packets/pedestrian_state_batch_packet.h"

#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"

namespace multiplayer::jak2::wire {

bool validate_packet(const PedestrianStateBatchPacket& packet) {
  if (packet.pedestrians.size() > core::kMaxPedestrians) {
    return false;
  }
  for (const auto& pedestrian : packet.pedestrians) {
    if (!platform::wire::valid_position_array(pedestrian.position) ||
        !platform::wire::valid_quaternion_array(pedestrian.quaternion) ||
        pedestrian.hit_points < 0 || pedestrian.hit_points > 255 || pedestrian.state_id > 43u ||
        pedestrian.flags > 3u || !valid_wire_player_reference(pedestrian.target_player_id)) {
      return false;
    }
  }
  return true;
}

PedestrianStateBatchPacket to_pedestrian_state_batch_packet(const core::TrafficSnapshot& snapshot) {
  PedestrianStateBatchPacket packet;
  packet.sample_time_ms = snapshot.sample_time_ms;
  packet.authority_revision = snapshot.authority_revision;
  packet.level_id = snapshot.level_id;
  packet.pedestrians.reserve(snapshot.pedestrians.size());
  for (const auto& pedestrian : snapshot.pedestrians) {
    packet.pedestrians.push_back({.net_id = pedestrian.net_id,
                                  .object_type = pedestrian.object_type,
                                  .appearance_mask = pedestrian.appearance_mask,
                                  .position = pedestrian.position,
                                  .quaternion = pedestrian.quaternion,
                                  .hit_points = pedestrian.hit_points,
                                  .state_id = pedestrian.state_id,
                                  .flags = pedestrian.state_flags,
                                  .target_player_id = pedestrian.target_player_id < kMPMaxPlayers
                                                          ? pedestrian.target_player_id
                                                          : kWireInvalidPlayerId,
                                  .vehicle_net_id = pedestrian.vehicle_net_id,
                                  .transport_id = pedestrian.transport_id,
                                  .transport_side = pedestrian.transport_side,
                                  .level_id = pedestrian.level_id});
  }
  return packet;
}

void from_packet(const PedestrianStateBatchPacket& packet, core::TrafficSnapshot& snapshot) {
  snapshot = {};
  snapshot.kind = core::TrafficSnapshot::Kind::PEDESTRIANS;
  snapshot.authority_revision = packet.authority_revision;
  snapshot.level_id = packet.level_id;
  snapshot.sample_time_ms = packet.sample_time_ms;
  snapshot.pedestrians.reserve(packet.pedestrians.size());
  for (const auto& [net_id, object_type, appearance_mask, position, quaternion, hit_points,
                    state_id, flags, target_player_id, vehicle_net_id, transport_id, transport_side,
                    level_id] : packet.pedestrians) {
    core::PedestrianState state;
    state.net_id = net_id;
    state.object_type = object_type;
    state.appearance_mask = appearance_mask;
    state.position = position;
    state.quaternion = quaternion;
    state.hit_points = hit_points;
    state.state_id = state_id;
    state.target_player_id =
        target_player_id < kMPMaxPlayers ? target_player_id : core::kInvalidPlayerId;
    state.vehicle_net_id = vehicle_net_id;
    state.transport_id = transport_id;
    state.transport_side = transport_side;
    state.state_flags = flags;
    state.level_id = level_id;
    snapshot.pedestrians.push_back(state);
  }
}

}  // namespace multiplayer::jak2::wire
