#include "game/multiplayer/jak2/wire/packets/airlock_state_batch_packet.h"

namespace multiplayer::jak2::wire {

bool validate_packet(const AirlockStateBatchPacket& packet) {
  if (packet.states.size() > core::kMaxAirlockStatesPerSnapshot) {
    return false;
  }
  for (const auto& state : packet.states) {
    if (state.airlock_aid == 0 || state.state_id > 3u || state.level_id > 0xffffu) {
      return false;
    }
  }
  return true;
}

AirlockStateBatchPacket to_packet(const core::AirlockSnapshot& snapshot) {
  AirlockStateBatchPacket packet;
  packet.states.reserve(snapshot.states.size());
  for (const auto& [airlock_aid, state_id, level_id, sequence] : snapshot.states) {
    packet.states.push_back({.airlock_aid = airlock_aid,
                             .state_id = state_id,
                             .level_id = level_id,
                             .sequence = sequence});
  }
  return packet;
}

void from_packet(const AirlockStateBatchPacket& packet, core::AirlockSnapshot& snapshot) {
  snapshot = {};
  snapshot.states.reserve(packet.states.size());
  for (const auto& [airlock_aid, state_id, level_id, sequence] : packet.states) {
    snapshot.states.push_back({.airlock_aid = airlock_aid,
                               .state_id = state_id,
                               .level_id = level_id,
                               .sequence = sequence});
  }
}

}  // namespace multiplayer::jak2::wire
