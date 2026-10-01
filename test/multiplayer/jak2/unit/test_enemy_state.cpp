#include "game/multiplayer/jak2/core/replication_state.h"
#include "gtest/gtest.h"

namespace {
using namespace multiplayer::jak2::core;

ApplyContext from(const PlayerId player_id, const Sequence sequence, const uint32_t now_ms = 0) {
  return {.sequence = sequence,
          .source = {.authenticated_player_id = player_id},
          .received_at_ms = now_ms};
}
}  // namespace

TEST(Jak2Replication, EnemyOwnershipHandoffUsesPerSourceSequences) {
  ReplicationState state;
  EnemySnapshot first = {.source_player_id = 1,
                         .enemies = {{.actor_id = 41, .owner_player_id = 1, .hit_points = 50}}};
  EnemySnapshot second = {.source_player_id = 2,
                          .enemies = {{.actor_id = 41, .owner_player_id = 2, .hit_points = 40}}};
  ASSERT_TRUE(state.entities().apply(first, from(1, 100, 1000)));
  ASSERT_TRUE(state.entities().apply(second, from(2, 1, 2000)));
  ASSERT_EQ(state.entities().enemies().enemies.size(), 1u);
  EXPECT_EQ(state.entities().enemies().enemies[0].owner_player_id, 2);
  EXPECT_EQ(state.entities().enemies().enemies[0].hit_points, 40);
  EXPECT_FALSE(state.entities().apply(first, from(1, 99, 3000)));
  state.entities().clear_enemy_source(2);
  EXPECT_TRUE(state.entities().enemies().enemies.empty());
}

TEST(Jak2Replication, EnemyGenerationValidatesBeforeMutationAndExpiresByArrival) {
  ReplicationState state;
  EnemySnapshot snapshot = {
      .source_player_id = 1,
      .enemies = {{.actor_id = 41, .owner_player_id = 1}, {.actor_id = 42, .owner_player_id = 1}}};
  ASSERT_TRUE(state.entities().apply(snapshot, from(1, 1, 1000)));
  snapshot.enemies = {{.actor_id = 41, .owner_player_id = 1, .hit_points = 17},
                      {.actor_id = 42, .owner_player_id = 1, .focus_player_id = 99}};
  EXPECT_FALSE(state.entities().apply(snapshot, from(1, 2, 1500)));
  EXPECT_EQ(state.entities().enemies().enemies[0].hit_points, 0);
  snapshot.enemies.resize(1);
  ASSERT_TRUE(state.entities().apply(snapshot, from(1, 2, 2000)));
  ASSERT_EQ(state.entities().enemies().enemies.size(), 1u);
  EXPECT_EQ(state.entities().enemies().enemies[0].hit_points, 17);
  state.expire(4001);
  EXPECT_TRUE(state.entities().enemies().enemies.empty());
}

TEST(Jak2Replication, EnemyAggregationPreservesEachParticipantsCapacity) {
  ReplicationState state;
  for (PlayerId source = 0; source < kMaxPlayers; ++source) {
    EnemySnapshot snapshot = {.source_player_id = source};
    for (uint32_t index = 0; index < kMaxEnemies; ++index)
      snapshot.enemies.push_back(
          {.actor_id = source * static_cast<uint32_t>(kMaxEnemies) + index + 1,
           .owner_player_id = source});
    ASSERT_TRUE(state.entities().apply(snapshot, from(source, 1, 1000)));
  }
  EXPECT_EQ(state.entities().enemies().enemies.size(), kMaxReplicatedEnemies);
  EXPECT_EQ(state.entities().enemies().enemies.front().actor_id, 1u);
  EXPECT_EQ(state.entities().enemies().enemies.back().actor_id, kMaxReplicatedEnemies);
  state.entities().depart(2);
  EXPECT_EQ(state.entities().enemies().enemies.size(), kMaxReplicatedEnemies - kMaxEnemies);
  state.expire(3001);
  EXPECT_TRUE(state.entities().enemies().enemies.empty());
}
