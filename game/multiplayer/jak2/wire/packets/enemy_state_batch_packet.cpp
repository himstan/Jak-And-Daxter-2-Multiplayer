#include "game/multiplayer/jak2/wire/packets/enemy_state_batch_packet.h"

#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"

namespace multiplayer::jak2::wire {

bool validate_packet(const EnemyStateBatchPacket& packet) {
  if (packet.enemies.size() > core::kMaxEnemies) {
    return false;
  }
  for (const auto& enemy : packet.enemies) {
    if (!platform::wire::valid_position_array(enemy.position) ||
        !platform::wire::valid_quaternion_array(enemy.quaternion) ||
        !valid_wire_player_reference(enemy.focus_player_id) || enemy.flags > 3u) {
      return false;
    }
  }
  return true;
}

EnemyStateBatchPacket to_packet(const core::EnemySnapshot& snapshot) {
  EnemyStateBatchPacket packet;
  packet.sample_time_ms = snapshot.sample_time_ms;
  packet.enemies.reserve(snapshot.enemies.size());
  for (const auto& enemy : snapshot.enemies) {
    packet.enemies.push_back(
        {.actor_id = enemy.actor_id,
         .position = enemy.position,
         .quaternion = enemy.quaternion,
         .hit_points = enemy.hit_points,
         .state_id = enemy.state_id,
         .focus_player_id =
             enemy.focus_player_id < kMPMaxPlayers ? enemy.focus_player_id : kWireInvalidPlayerId,
         .flags = static_cast<uint8_t>((enemy.attack_active ? 1u : 0u) | (enemy.aggro ? 2u : 0u))});
  }
  return packet;
}

void from_packet(const EnemyStateBatchPacket& packet, core::EnemySnapshot& snapshot) {
  snapshot = {};
  snapshot.sample_time_ms = packet.sample_time_ms;
  snapshot.enemies.reserve(packet.enemies.size());
  for (const auto& [actor_id, position, quaternion, hit_points, state_id, focus_player_id, flags] :
       packet.enemies) {
    core::EnemyState state;
    state.actor_id = actor_id;
    state.position = position;
    state.quaternion = quaternion;
    state.hit_points = hit_points;
    state.state_id = state_id;
    state.focus_player_id =
        focus_player_id < kMPMaxPlayers ? focus_player_id : core::kInvalidPlayerId;
    state.attack_active = (flags & 1u) != 0;
    state.aggro = (flags & 2u) != 0;
    snapshot.enemies.push_back(state);
  }
}

}  // namespace multiplayer::jak2::wire
