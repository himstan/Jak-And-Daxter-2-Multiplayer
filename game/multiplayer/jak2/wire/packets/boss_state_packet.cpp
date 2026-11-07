#include "game/multiplayer/jak2/wire/packets/boss_state_packet.h"

#include "game/multiplayer/jak2/core/validation.h"

namespace multiplayer::jak2::wire {

bool validate_packet(const BossStatePacket& packet) {
  core::BossState state = {};
  from_packet(packet, state);
  return core::valid_boss_state(state) && platform::wire::valid_unit(packet.health) &&
         platform::wire::valid_unit(packet.shield) &&
         platform::wire::valid_position_array(packet.position) &&
         platform::wire::valid_quaternion_array(packet.quaternion) &&
         platform::wire::valid_position_array(packet.anchor_position) &&
         platform::wire::valid_quaternion_array(packet.anchor_quaternion) &&
         platform::wire::valid_position_array(packet.trajectory_source) &&
         platform::wire::valid_position_array(packet.trajectory_destination) &&
         platform::wire::valid_trajectory_duration(packet.trajectory_duration);
}

BossStatePacket to_boss_state_packet(const core::BossState& state) {
  return {.sample_time_ms = state.sample_time_ms,
          .kind = static_cast<uint8_t>(state.kind),
          .state_id = state.state_id,
          .stage = state.stage,
          .target_player_slot = state.target_player_slot < core::kMaxPlayers
                                    ? state.target_player_slot
                                    : kWireInvalidPlayerSlot,
          .draw_force_fade = state.draw_force_fade,
          .health = state.health,
          .shield = state.shield,
          .state_entry = state.state_entry,
          .position = state.position,
          .quaternion = state.quaternion,
          .anchor_position = state.anchor_position,
          .anchor_quaternion = state.anchor_quaternion,
          .trajectory_source = state.trajectory_source,
          .trajectory_destination = state.trajectory_destination,
          .trajectory_duration = state.trajectory_duration,
          .trajectory_age = state.trajectory_age};
}

void from_packet(const BossStatePacket& packet, core::BossState& state) {
  state = {.kind = static_cast<core::BossState::Kind>(packet.kind),
           .state_id = packet.state_id,
           .stage = packet.stage,
           .target_player_slot = packet.target_player_slot < core::kMaxPlayers
                                     ? packet.target_player_slot
                                     : core::kInvalidPlayerSlot,
           .draw_force_fade = packet.draw_force_fade,
           .health = packet.health,
           .shield = packet.shield,
           .state_entry = packet.state_entry,
           .position = packet.position,
           .quaternion = packet.quaternion,
           .anchor_position = packet.anchor_position,
           .anchor_quaternion = packet.anchor_quaternion,
           .trajectory_source = packet.trajectory_source,
           .trajectory_destination = packet.trajectory_destination,
           .trajectory_duration = packet.trajectory_duration,
           .trajectory_age = packet.trajectory_age,
           .sample_time_ms = packet.sample_time_ms};
}

}  // namespace multiplayer::jak2::wire
