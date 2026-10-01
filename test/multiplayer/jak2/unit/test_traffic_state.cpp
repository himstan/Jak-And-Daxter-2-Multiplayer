#include "game/multiplayer/jak2/core/replication_state.h"
#include "gtest/gtest.h"

namespace {
using namespace multiplayer::jak2::core;

TrafficAuthority authority_for(const PlayerId source, const uint32_t revision = 1) {
  TrafficAuthority authority = {.revision = revision};
  authority.assignments.fill(kInvalidPlayerId);
  authority.assignments[source] = source;
  return authority;
}

ApplyContext from(const PlayerId source, const Sequence sequence, const uint32_t now_ms = 0) {
  return {.sequence = sequence,
          .source = {.authenticated_player_id = source},
          .received_at_ms = now_ms};
}

VehicleState vehicle(const EntityId net_id, const float x = 0.0f) {
  VehicleState result = {.net_id = net_id, .position = {x, 2.0f, 3.0f}};
  result.quaternion[3] = 1.0f;
  return result;
}
}  // namespace

TEST(Jak2Replication, TrafficGenerationAtomicallyReplacesPreviousGeneration) {
  ReplicationState state;
  const auto authority = authority_for(1);
  ASSERT_TRUE(state.traffic().apply(authority, {.source = {.from_host = true}}));
  TrafficSnapshot snapshot = {.kind = TrafficSnapshot::Kind::VEHICLES,
                              .source_player_id = 1,
                              .authority_revision = authority.revision,
                              .vehicles = {vehicle(0x21000001u, 10), vehicle(0x21000002u, 20)}};
  ASSERT_TRUE(state.traffic().apply(snapshot, from(1, 1, 1000)));
  snapshot.vehicles = {vehicle(0x21000001u, 11)};
  ASSERT_TRUE(state.traffic().apply(snapshot, from(1, 2, 1033)));
  ASSERT_EQ(state.traffic().aggregate().vehicles.size(), 1u);
  EXPECT_FLOAT_EQ(state.traffic().aggregate().vehicles[0].position[0], 11.0f);
}

TEST(Jak2Replication, TrafficRejectsInvalidBatchBeforeMutation) {
  ReplicationState state;
  ASSERT_TRUE(state.traffic().apply(authority_for(1), {.source = {.from_host = true}}));
  TrafficSnapshot snapshot = {.kind = TrafficSnapshot::Kind::VEHICLES,
                              .source_player_id = 1,
                              .authority_revision = 1,
                              .vehicles = {vehicle(0x21000001u)}};
  ASSERT_TRUE(state.traffic().apply(snapshot, from(1, 1)));
  snapshot.vehicles.push_back(vehicle(0));
  EXPECT_FALSE(state.traffic().apply(snapshot, from(1, 2)));
  ASSERT_EQ(state.traffic().aggregate().vehicles.size(), 1u);
  EXPECT_EQ(state.traffic().aggregate().vehicles[0].net_id, 0x21000001u);
}

TEST(Jak2Replication, TrafficUsesPerKindSequencesAndExpiresByArrival) {
  ReplicationState state;
  ASSERT_TRUE(state.traffic().apply(authority_for(1), {.source = {.from_host = true}}));
  TrafficSnapshot vehicles = {.kind = TrafficSnapshot::Kind::VEHICLES,
                              .source_player_id = 1,
                              .authority_revision = 1,
                              .vehicles = {vehicle(0x21000001u)}};
  TrafficSnapshot pedestrians = {.kind = TrafficSnapshot::Kind::PEDESTRIANS,
                                 .source_player_id = 1,
                                 .authority_revision = 1,
                                 .pedestrians = {{.net_id = 0x11000001u}}};
  ASSERT_TRUE(state.traffic().apply(vehicles, from(1, 4, 1000)));
  ASSERT_TRUE(state.traffic().apply(pedestrians, from(1, 1, 1500)));
  EXPECT_FALSE(state.traffic().apply(vehicles, from(1, 4, 2000)));
  state.expire(3001);
  EXPECT_TRUE(state.traffic().aggregate().vehicles.empty());
  EXPECT_EQ(state.traffic().aggregate().pedestrians.size(), 1u);
  state.expire(3501);
  EXPECT_TRUE(state.traffic().aggregate().pedestrians.empty());
}

TEST(Jak2Replication, TrafficSelectionAndAuthorityRevisionClearOldState) {
  ReplicationState state;
  ASSERT_TRUE(state.traffic().apply(authority_for(1), {.source = {.from_host = true}}));
  ASSERT_TRUE(state.traffic().select_authority(1));
  TrafficSnapshot snapshot = {.kind = TrafficSnapshot::Kind::VEHICLES,
                              .source_player_id = 1,
                              .authority_revision = 1,
                              .vehicles = {vehicle(0x21000001u)}};
  ASSERT_TRUE(state.traffic().apply(snapshot, from(1, 1)));
  EXPECT_EQ(state.traffic().selected_snapshot().vehicles.size(), 1u);
  EXPECT_TRUE(state.traffic().apply(authority_for(1), {.source = {.from_host = true}}));
  EXPECT_EQ(state.traffic().selected_snapshot().vehicles.size(), 1u);
  EXPECT_TRUE(state.traffic().apply(authority_for(1, 2), {.source = {.from_host = true}}));
  EXPECT_TRUE(state.traffic().aggregate().vehicles.empty());
}

TEST(Jak2Replication, TrafficLevelChangeDropsOtherKindFromPreviousLevel) {
  ReplicationState state;
  ASSERT_TRUE(state.traffic().apply(authority_for(1), {.source = {.from_host = true}}));
  TrafficSnapshot pedestrians = {.kind = TrafficSnapshot::Kind::PEDESTRIANS,
                                 .source_player_id = 1,
                                 .authority_revision = 1,
                                 .level_id = 10,
                                 .pedestrians = {{.net_id = 0x11000001u}}};
  ASSERT_TRUE(state.traffic().apply(pedestrians, from(1, 1)));
  TrafficSnapshot vehicles = {.kind = TrafficSnapshot::Kind::VEHICLES,
                              .source_player_id = 1,
                              .authority_revision = 1,
                              .level_id = 20,
                              .vehicles = {vehicle(0x21000001u)}};
  ASSERT_TRUE(state.traffic().apply(vehicles, from(1, 1)));
  EXPECT_TRUE(state.traffic().aggregate().pedestrians.empty());
  EXPECT_EQ(state.traffic().aggregate().vehicles.size(), 1u);
}
