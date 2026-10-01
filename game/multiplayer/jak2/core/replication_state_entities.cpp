#include <algorithm>

#include "game/multiplayer/jak2/core/replication_state.h"
#include "game/multiplayer/jak2/core/validation.h"
#include "game/multiplayer/platform/core/sequence.h"

namespace multiplayer::jak2::core {

void EntityReplicationState::reset() {
  enemies_ = {};
  enemy_snapshots_ = {};
  bosses_ = {};
  airlock_snapshots_ = {};
  airlock_sequences_ = {};
}

void EntityReplicationState::expire(const uint64_t now_ms) {
  bool enemies_changed = false;
  for (auto& snapshot : enemy_snapshots_) {
    enemies_changed |= std::erase_if(snapshot.enemies, [now_ms](const auto& enemy) {
                         return now_ms - enemy.received_time_ms > 2000;
                       }) != 0;
  }
  if (enemies_changed)
    rebuild_enemies();
  for (auto& boss : bosses_) {
    if (boss.active != 0 && now_ms - boss.received_time_ms > 2000)
      boss.active = 0;
  }
  for (auto& snapshot : airlock_snapshots_) {
    if (snapshot.source_player_id != kInvalidPlayerId && now_ms - snapshot.received_time_ms > 2000)
      snapshot = {};
  }
}

bool EntityReplicationState::apply(const EnemySnapshot& incoming, const ApplyContext& context) {
  if (incoming.enemies.size() > kMaxEnemies ||
      !std::ranges::all_of(incoming.enemies, valid_enemy_state)) {
    return false;
  }
  const auto source_player_id = snapshot_source(incoming.source_player_id, context.source);
  if (!valid_index(source_player_id, kMaxPlayers) ||
      (!context.source.from_host && context.source.authenticated_player_id != kInvalidPlayerId &&
       source_player_id != context.source.authenticated_player_id) ||
      !platform::sequence_is_newer(context.sequence, enemy_snapshots_[source_player_id].sequence)) {
    return false;
  }
  auto& snapshot = enemy_snapshots_[source_player_id];
  snapshot = incoming;
  for (size_t index = 0; index < incoming.enemies.size(); ++index) {
    auto enemy = incoming.enemies[index];
    for (PlayerId other = 0; other < kMaxPlayers; ++other) {
      if (other != source_player_id) {
        std::erase_if(enemy_snapshots_[other].enemies,
                      [&](const auto& cached) { return cached.actor_id == enemy.actor_id; });
      }
    }
    enemy.owner_player_id = source_player_id;
    enemy.last_sequence = context.sequence;
    enemy.sample_time_ms = incoming.sample_time_ms;
    enemy.received_time_ms = context.received_at_ms;
    snapshot.enemies[index] = enemy;
  }
  snapshot.source_player_id = source_player_id;
  snapshot.sequence = context.sequence;
  rebuild_enemies();
  enemies_.sample_time_ms = incoming.sample_time_ms;
  return true;
}

bool EntityReplicationState::apply(const BossState& state, const ApplyContext& context) {
  if (static_cast<size_t>(state.kind) >= bosses_.size() || !valid_boss_state(state))
    return false;
  auto& current = bosses_[static_cast<size_t>(state.kind)];
  if (!platform::sequence_is_newer(context.sequence, current.sequence))
    return false;
  current = state;
  current.sequence = context.sequence;
  current.received_time_ms = context.received_at_ms;
  return true;
}

bool EntityReplicationState::apply(const AirlockSnapshot& incoming, const ApplyContext& context) {
  if (incoming.states.size() > kMaxAirlockStatesPerSnapshot ||
      !std::ranges::all_of(incoming.states, [](const auto& state) {
        return state.airlock_aid != 0 && state.state_id <= 3;
      })) {
    return false;
  }
  const auto source_player_id = snapshot_source(incoming.source_player_id, context.source);
  if (!valid_index(source_player_id, kMaxPlayers) || source_player_id == context.local_player_id ||
      !source_allows_player(context.source, source_player_id) ||
      !platform::sequence_is_newer(context.sequence, airlock_sequences_[source_player_id])) {
    return false;
  }
  auto& snapshot = airlock_snapshots_[source_player_id];
  snapshot = incoming;
  snapshot.source_player_id = source_player_id;
  snapshot.sequence = context.sequence;
  airlock_sequences_[source_player_id] = context.sequence;
  snapshot.received_time_ms = context.received_at_ms;
  return true;
}

void EntityReplicationState::depart(const PlayerId player_id) {
  if (!valid_index(player_id, kMaxPlayers))
    return;
  airlock_sequences_[player_id] = 0;
  airlock_snapshots_[player_id] = {};
  clear_enemy_source(player_id);
}

void EntityReplicationState::clear_enemy_source(const PlayerId player_id) {
  if (!valid_index(player_id, kMaxPlayers))
    return;
  enemy_snapshots_[player_id] = {};
  rebuild_enemies();
}

void EntityReplicationState::rebuild_enemies() {
  const auto sample_time_ms = enemies_.sample_time_ms;
  enemies_ = {};
  enemies_.sample_time_ms = sample_time_ms;
  for (PlayerId source = 0; source < kMaxPlayers; ++source) {
    const auto& snapshot = enemy_snapshots_[source];
    for (const auto& enemy : snapshot.enemies) {
      const auto existing = std::ranges::find_if(enemies_.enemies, [&](const EnemyState& state) {
        return state.actor_id == enemy.actor_id;
      });
      if (existing == enemies_.enemies.end() && enemies_.enemies.size() < kMaxReplicatedEnemies) {
        enemies_.enemies.push_back(enemy);
      } else if (existing != enemies_.enemies.end() &&
                 platform::sequence_is_current_or_newer(enemy.last_sequence,
                                                        existing->last_sequence)) {
        *existing = enemy;
      }
    }
    if (snapshot.sequence >= enemies_.sequence) {
      enemies_.source_player_id = snapshot.source_player_id;
      enemies_.sequence = snapshot.sequence;
    }
  }
}

}  // namespace multiplayer::jak2::core
