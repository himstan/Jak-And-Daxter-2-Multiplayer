#include "game/multiplayer/jak2/wire/packets/turret_state_packet.h"

namespace multiplayer::jak2::wire {

bool validate_packet(const TurretStatePacket& packet) {
  return platform::wire::valid_angle(packet.rotation_y) &&
         platform::wire::valid_angle(packet.rotation_x);
}

TurretStatePacket to_packet(const core::TurretState& state) {
  return {.turret_aid = state.turret_aid,
          .rotation_y = platform::wire::canonical_angle(state.rotation_y),
          .rotation_x = platform::wire::canonical_angle(state.rotation_x),
          .path_progress = state.path_progress};
}

void from_packet(const TurretStatePacket& packet, core::TurretState& state) {
  state = {};
  state.turret_aid = packet.turret_aid;
  state.rotation_y = packet.rotation_y;
  state.rotation_x = packet.rotation_x;
  state.path_progress = packet.path_progress;
}

}  // namespace multiplayer::jak2::wire
