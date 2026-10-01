#include "game/multiplayer/jak2/wire/packets/boss_state_packet.h"

namespace multiplayer::jak2::wire {

bool validate_packet(const PalaceSquidStatePacket& packet) {
  return packet.active <= 1u && packet.state_id <= 15u &&
         platform::wire::valid_unit(packet.shield_hit_points) &&
         valid_wire_player_reference(packet.target_player_id) && packet.draw_force_fade <= 128u &&
         platform::wire::valid_position_array(packet.position) &&
         platform::wire::valid_quaternion_array(packet.quaternion) &&
         platform::wire::valid_position_array(packet.root_position) &&
         platform::wire::valid_quaternion_array(packet.root_quaternion) &&
         platform::wire::valid_position_array(packet.trajectory_source) &&
         platform::wire::valid_position_array(packet.trajectory_destination) &&
         platform::wire::valid_trajectory_duration(packet.trajectory_duration);
}

bool validate_packet(const WidowStatePacket& packet) {
  return packet.active <= 1u && packet.state_id <= 25u &&
         platform::wire::valid_position_array(packet.position) &&
         platform::wire::valid_quaternion_array(packet.quaternion);
}

PalaceSquidStatePacket to_palace_squid_state_packet(const core::BossState& state) {
  return {.sample_time_ms = state.sample_time_ms,
          .active = state.active,
          .state_id = state.state_id,
          .stage = state.stage,
          .hit_points = state.hit_points,
          .shield_hit_points = state.shield_hit_points,
          .target_player_id = state.target_player_id < core::kMaxPlayers ? state.target_player_id
                                                                         : kWireInvalidPlayerId,
          .draw_force_fade = state.draw_force_fade,
          .action_sequence = state.action_sequence,
          .position = state.position,
          .quaternion = state.quaternion,
          .root_position = state.root_position,
          .root_quaternion = state.root_quaternion,
          .trajectory_source = state.trajectory_source,
          .trajectory_destination = state.trajectory_destination,
          .trajectory_duration = state.trajectory_duration,
          .trajectory_age = state.trajectory_age};
}

WidowStatePacket to_widow_state_packet(const core::BossState& state) {
  return {.sample_time_ms = state.sample_time_ms,
          .active = state.active,
          .state_id = state.state_id,
          .position = state.position,
          .quaternion = state.quaternion};
}

void from_packet(const PalaceSquidStatePacket& packet, core::BossState& state) {
  state = {};
  state.kind = core::BossState::Kind::PALACE_SQUID;
  state.sample_time_ms = packet.sample_time_ms;
  state.active = packet.active;
  state.state_id = packet.state_id;
  state.stage = packet.stage;
  state.hit_points = packet.hit_points;
  state.shield_hit_points = packet.shield_hit_points;
  state.target_player_id = packet.target_player_id < core::kMaxPlayers ? packet.target_player_id
                                                                       : core::kInvalidPlayerId;
  state.draw_force_fade = packet.draw_force_fade;
  state.action_sequence = packet.action_sequence;
  state.position = packet.position;
  state.quaternion = packet.quaternion;
  state.root_position = packet.root_position;
  state.root_quaternion = packet.root_quaternion;
  state.trajectory_source = packet.trajectory_source;
  state.trajectory_destination = packet.trajectory_destination;
  state.trajectory_duration = packet.trajectory_duration;
  state.trajectory_age = packet.trajectory_age;
}

void from_packet(const WidowStatePacket& packet, core::BossState& state) {
  state = {};
  state.kind = core::BossState::Kind::WIDOW;
  state.sample_time_ms = packet.sample_time_ms;
  state.active = packet.active;
  state.state_id = packet.state_id;
  state.position = packet.position;
  state.quaternion = packet.quaternion;
}

}  // namespace multiplayer::jak2::wire
