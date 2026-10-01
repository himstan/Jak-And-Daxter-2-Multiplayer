#include "game/multiplayer/jak2/wire/packets/player_state_packet.h"

#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"

namespace multiplayer::jak2::wire {

bool validate_packet(const PlayerStatePacket& packet) {
  return packet.activity <= core::PlayerActivity::IN_GAME &&
         platform::wire::valid_position_array(packet.position) &&
         platform::wire::valid_angle(packet.angle) &&
         platform::wire::valid_linear_velocity_array(packet.velocity) && packet.state_id <= 0xffu &&
         core::valid_player_level_state(packet.levels) && packet.vehicle_seat <= 3u &&
         packet.equipped_weapon <= 4u && packet.flags <= 0x3fu &&
         platform::wire::valid_angle(packet.camera_angle_y) && packet.action_sequence <= 0xffu &&
         packet.action_state_id <= 0xffu &&
         valid_wire_player_reference(packet.riding_along_player_id) &&
         packet.mission_flags <= 0x03u && packet.visual_secrets <= 0x07u;
}

PlayerStatePacket to_packet(const core::PlayerState& state) {
  return {.sample_time_ms = state.sample_time_ms,
          .activity = state.activity,
          .position = state.position,
          .angle = platform::wire::canonical_angle(state.angle),
          .velocity = state.velocity,
          .state_id = static_cast<uint8_t>(state.state_id & 0xffu),
          .levels = state.levels,
          .darkjak_stage = state.darkjak_stage,
          .buttons = state.buttons,
          .leftx = state.leftx,
          .lefty = state.lefty,
          .flags = pack_player_state_flags(state.spectator_only, state.scene_active,
                                           state.turret_active, state.respawn_flags),
          .camera_angle_y = platform::wire::canonical_angle(state.camera_angle_y),
          .vehicle_id = state.vehicle_id,
          .vehicle_seat = state.vehicle_seat,
          .equipped_weapon = state.equipped_weapon,
          .action_sequence = static_cast<uint8_t>(state.action_sequence & 0xffu),
          .action_state_id = static_cast<uint8_t>(state.action_state_id & 0xffu),
          .riding_along_player_id = state.riding_along_player_id < kMPMaxPlayers
                                        ? state.riding_along_player_id
                                        : kWireInvalidPlayerId,
          .mission_flags = state.mission_flags,
          .visual_secrets = state.visual_secrets};
}

void from_packet(const PlayerStatePacket& packet, core::PlayerState& state) {
  state = {};
  state.activity = packet.activity;
  state.state_ready = packet.activity != core::PlayerActivity::UNAVAILABLE;
  state.position = packet.position;
  state.angle = packet.angle;
  state.velocity = packet.velocity;
  state.state_id = packet.state_id;
  state.levels = packet.levels;
  state.darkjak_stage = packet.darkjak_stage;
  state.buttons = packet.buttons;
  state.leftx = packet.leftx;
  state.lefty = packet.lefty;
  state.spectator_only = is_player_state_flag_spectator(packet.flags);
  state.scene_active = is_player_state_flag_scene_active(packet.flags);
  state.turret_active = is_player_state_flag_turret_active(packet.flags);
  state.respawn_flags = has_player_state_flag_respawn_flags(packet.flags);
  state.camera_angle_y = packet.camera_angle_y;
  state.vehicle_id = packet.vehicle_id;
  state.vehicle_seat = packet.vehicle_seat;
  state.equipped_weapon = packet.equipped_weapon;
  state.action_sequence = packet.action_sequence;
  state.action_state_id = packet.action_state_id;
  state.riding_along_player_id = packet.riding_along_player_id < kMPMaxPlayers
                                     ? packet.riding_along_player_id
                                     : core::kInvalidPlayerId;
  state.mission_flags = packet.mission_flags;
  state.visual_secrets = packet.visual_secrets;
  state.sample_time_ms = packet.sample_time_ms;
}

}  // namespace multiplayer::jak2::wire
