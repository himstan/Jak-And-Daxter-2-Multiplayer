#include "game/multiplayer/jak2/wire/packets/traffic_authority_state_packet.h"

namespace multiplayer::jak2::wire {

bool validate_packet(const TrafficAuthorityStatePacket& packet) {
  if (packet.revision == 0) {
    return false;
  }
  for (const uint8_t assignment : packet.assignments) {
    if (!valid_wire_player_reference(assignment)) {
      return false;
    }
  }
  return true;
}

TrafficAuthorityStatePacket to_packet(const core::TrafficAuthority& authority) {
  TrafficAuthorityStatePacket packet;
  packet.revision = authority.revision;
  for (size_t index = 0; index < packet.assignments.size(); ++index) {
    packet.assignments[index] = authority.assignments[index] < core::kMaxPlayers
                                    ? authority.assignments[index]
                                    : kWireInvalidPlayerId;
  }
  return packet;
}

void from_packet(const TrafficAuthorityStatePacket& packet, core::TrafficAuthority& authority) {
  authority = {};
  authority.revision = packet.revision;
  for (size_t index = 0; index < authority.assignments.size(); ++index) {
    authority.assignments[index] = packet.assignments[index] < core::kMaxPlayers
                                       ? packet.assignments[index]
                                       : core::kInvalidPlayerId;
  }
}

}  // namespace multiplayer::jak2::wire
