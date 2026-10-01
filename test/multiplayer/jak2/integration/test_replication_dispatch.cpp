#include "game/multiplayer/jak2/core/replication_state.h"
#include "gtest/gtest.h"

namespace {
using namespace multiplayer::jak2::core;

ApplyContext from(const PlayerId source, const Sequence sequence, const bool host = false) {
  return {.sequence = sequence, .source = {.authenticated_player_id = source, .from_host = host}};
}
}  // namespace

TEST(Jak2AdapterIntegration, TypedDomainsApplyEveryGameplayPayload) {
  ReplicationState state;
  PlayerState player = {.player_id = 1};
  EXPECT_TRUE(state.players().apply(player, from(1, 1)));

  TrafficAuthority authority = {.revision = 1};
  authority.assignments.fill(kInvalidPlayerId);
  authority.assignments[1] = 1;
  EXPECT_TRUE(state.traffic().apply(authority, from(0, 1, true)));

  WorldState world = {.clock = 17};
  EXPECT_TRUE(state.world().apply(world, from(0, 1, true)));

  EnemySnapshot enemies = {.source_player_id = 1,
                           .enemies = {{.actor_id = 7, .owner_player_id = 1}}};
  EXPECT_TRUE(state.entities().apply(enemies, from(1, 1)));

  GameEventBatch events = {.events = {{.event_id = 2, .source_player_id = 1}}};
  EXPECT_TRUE(state.events().apply(events, from(1, 1)));
  EXPECT_EQ(state.players().players()[1].player_id, 1);
  EXPECT_EQ(state.traffic().authority().revision, 1u);
  EXPECT_EQ(state.world().world().clock, 17u);
  EXPECT_EQ(state.entities().enemies().enemies.size(), 1u);
  EXPECT_EQ(state.events().events().size(), 1u);
}

TEST(Jak2AdapterIntegration, HostMayRelayCanonicalTrafficForAuthenticatedSource) {
  ReplicationState state;
  TrafficAuthority authority = {.revision = 1};
  authority.assignments.fill(kInvalidPlayerId);
  authority.assignments[2] = 2;
  ASSERT_TRUE(state.traffic().apply(authority, from(0, 1, true)));
  TrafficSnapshot traffic = {.kind = TrafficSnapshot::Kind::VEHICLES,
                             .source_player_id = 2,
                             .authority_revision = 1,
                             .vehicles = {{.net_id = 0x22000001u}}};
  traffic.vehicles[0].quaternion[3] = 1.0f;
  EXPECT_FALSE(state.traffic().apply(traffic, from(1, 1)));
  EXPECT_TRUE(state.traffic().apply(traffic, from(0, 1, true)));
  EXPECT_EQ(state.traffic().vehicle_source(0x22000001u), 2);
}

TEST(Jak2AdapterIntegration, BootstrapAndLiveWorldShareOneDomain) {
  ReplicationState state;
  BootstrapState bootstrap = {};
  bootstrap.world.money = 5;
  ASSERT_TRUE(state.apply_bootstrap(bootstrap, 1));
  WorldState live = {.money = 99, .clock = 20};
  ASSERT_TRUE(state.world().apply(live, from(0, 2, true)));
  EXPECT_FLOAT_EQ(state.world().bootstrap().world.money, 5);
  EXPECT_FLOAT_EQ(state.world().world().money, 5);
  EXPECT_EQ(state.world().world().clock, 20u);
}

TEST(Jak2AdapterIntegration, AirlocksIgnoreLocalLoopbackSource) {
  ReplicationState state;
  AirlockSnapshot airlocks = {.source_player_id = 1, .states = {{.airlock_aid = 7, .state_id = 1}}};
  EXPECT_FALSE(state.entities().apply(
      airlocks, {.sequence = 1, .source = {.authenticated_player_id = 1}, .local_player_id = 1}));
  EXPECT_TRUE(state.entities().apply(
      airlocks, {.sequence = 1, .source = {.authenticated_player_id = 1}, .local_player_id = 2}));
}
