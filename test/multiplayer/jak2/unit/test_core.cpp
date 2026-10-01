#include <limits>
#include <vector>

#include "game/multiplayer/jak2/application/presentation_runtime.h"
#include "game/multiplayer/jak2/core/replication_state.h"
#include "game/multiplayer/jak2/core/validation.h"
#include "gtest/gtest.h"

TEST(Jak2Protocol, ValidationUsesExplicitBoundsAndFiniteValues) {
  EXPECT_TRUE(multiplayer::jak2::core::finite(1.0f));
  EXPECT_FALSE(multiplayer::jak2::core::finite(std::numeric_limits<float>::infinity()));
  EXPECT_TRUE(multiplayer::jak2::core::count_fits(8, 8));
  EXPECT_FALSE(multiplayer::jak2::core::count_fits(9, 8));
  EXPECT_TRUE(multiplayer::jak2::core::valid_index(7, 8));
  EXPECT_FALSE(multiplayer::jak2::core::valid_index(8, 8));
}
TEST(Jak2Protocol, PlayerMessagesCommitOnlyWhenFresh) {
  using namespace multiplayer::jak2::core;
  ReplicationState state;
  PlayerState player = {};
  player.player_id = 2;
  player.state_id = 17;
  const SourceContext source{.authenticated_player_id = 2};
  ASSERT_TRUE(state.participants().apply(player, {.sequence = 4, .source = source}));
  EXPECT_EQ(state.participants().players()[2].state_id, 17u);
  player.state_id = 99;
  EXPECT_FALSE(state.participants().apply(player, {.sequence = 3, .source = source}));
  EXPECT_EQ(state.participants().players()[2].state_id, 17u);
}

TEST(Jak2Protocol, InvalidFiniteValuesDoNotPartiallyCommit) {
  using namespace multiplayer::jak2::core;
  ReplicationState state;
  PlayerState player = {};
  player.player_id = 4;
  player.state_id = 22;
  player.position[1] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(state.participants().apply(
      player, {.sequence = 8, .source = {.authenticated_player_id = 4}}));
  EXPECT_EQ(state.participants().players()[4].state_id, 0u);
  EXPECT_EQ(state.participants().players()[4].last_sequence, 0u);
}

TEST(Jak2Protocol, SourceSequencesMergeWithoutCrossPeerOverwrite) {
  using namespace multiplayer::jak2::core;
  ReplicationState state;

  multiplayer::jak2::core::TrafficAuthority authority = {};
  authority.revision = 1;
  authority.assignments.fill(multiplayer::jak2::core::kInvalidPlayerId);
  authority.assignments[1] = 1;
  authority.assignments[2] = 2;
  ASSERT_TRUE(state.traffic().apply(authority, {.sequence = 1, .source = {.from_host = true}}));

  multiplayer::jak2::core::TrafficSnapshot first = {};
  first.kind = multiplayer::jak2::core::TrafficSnapshot::Kind::VEHICLES;
  first.source_player_id = 1;
  first.authority_revision = authority.revision;
  first.vehicles.push_back({.net_id = 0x21000001u});
  ASSERT_TRUE(
      state.traffic().apply(first, {.sequence = 1, .source = {.authenticated_player_id = 1}}));

  multiplayer::jak2::core::TrafficSnapshot second = {};
  second.kind = multiplayer::jak2::core::TrafficSnapshot::Kind::VEHICLES;
  second.source_player_id = 2;
  second.authority_revision = authority.revision;
  second.vehicles.push_back({.net_id = 0x22000001u});
  ASSERT_TRUE(
      state.traffic().apply(second, {.sequence = 1, .source = {.authenticated_player_id = 2}}));

  first.sequence = 2;
  first.vehicles.clear();
  ASSERT_TRUE(
      state.traffic().apply(first, {.sequence = 2, .source = {.authenticated_player_id = 1}}));
  ASSERT_EQ(state.traffic().aggregate().vehicles.size(), 1u);
  EXPECT_EQ(state.traffic().aggregate().vehicles[0].net_id, 0x22000001u);
  state.traffic().clear_source(1);
  ASSERT_EQ(state.traffic().aggregate().vehicles.size(), 1u);
  EXPECT_EQ(state.traffic().aggregate().vehicles[0].net_id, 0x22000001u);
}

TEST(Jak2Protocol, EnemyBatchesKeepIndependentAuthoritativeSources) {
  using namespace multiplayer::jak2::core;
  ReplicationState state;
  multiplayer::jak2::core::EnemySnapshot first = {};
  first.source_player_id = 1;
  first.enemies.push_back({.actor_id = 41, .owner_player_id = 1});
  multiplayer::jak2::core::EnemySnapshot second = {};
  second.source_player_id = 2;
  second.enemies.push_back({.actor_id = 42, .owner_player_id = 2});

  ASSERT_TRUE(
      state.entities().apply(first, {.sequence = 1, .source = {.authenticated_player_id = 1}}));
  ASSERT_TRUE(
      state.entities().apply(second, {.sequence = 1, .source = {.authenticated_player_id = 2}}));
  ASSERT_EQ(state.entities().enemies().enemies.size(), 2u);
  EXPECT_EQ(state.entities().enemies().enemies[0].last_sequence, 1u);
  EXPECT_EQ(state.entities().enemies().enemies[1].last_sequence, 1u);
}

TEST(Jak2Protocol, SnapshotTimelineUsesGoalScaleForTeleportDetection) {
  using multiplayer::jak2::application::kGoalUnitsPerMeter;
  using multiplayer::platform::replication::SnapshotTimeline;
  SnapshotTimeline timeline;
  ASSERT_TRUE(timeline.push({.sample_time_ms = 1000,
                             .position = {0.0f, 0.0f, 0.0f},
                             .velocity = {4096.0f, 0.0f, 0.0f},
                             .velocity_valid = true},
                            1100, 15.0f * kGoalUnitsPerMeter));
  ASSERT_TRUE(timeline.push({.sample_time_ms = 2000,
                             .position = {4096.0f, 0.0f, 0.0f},
                             .velocity = {4096.0f, 0.0f, 0.0f},
                             .velocity_valid = true},
                            2100, 15.0f * kGoalUnitsPerMeter));

  const auto ordinary_motion = timeline.present(2600, 1000, 33, 1500, 250);
  ASSERT_TRUE(ordinary_motion.valid);
  EXPECT_NEAR(ordinary_motion.position[0], 2048.0f, 0.01f);

  ASSERT_TRUE(
      timeline.push({.sample_time_ms = 3000, .position = {25.0f * kGoalUnitsPerMeter, 0.0f, 0.0f}},
                    3100, 15.0f * kGoalUnitsPerMeter));
  const auto teleported = timeline.present(3233, 1000, 33, 1500, 250);
  ASSERT_TRUE(teleported.valid);
  EXPECT_FLOAT_EQ(teleported.position[0], 25.0f * kGoalUnitsPerMeter);
}
