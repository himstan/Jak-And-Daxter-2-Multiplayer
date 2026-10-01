#include <limits>

#include "game/multiplayer/jak2/core/replication_state.h"
#include "gtest/gtest.h"

namespace {
using namespace multiplayer::jak2::core;

ApplyContext from(const PlayerId source,
                  const Sequence sequence,
                  const uint64_t now_ms = 0,
                  const PlayerId local = kInvalidPlayerId) {
  return {.sequence = sequence,
          .source = {.authenticated_player_id = source},
          .received_at_ms = now_ms,
          .local_player_id = local};
}

PlayerIdentity identity(const PlayerId player_id) {
  PlayerIdentity result = {.player_id = player_id,
                           .character = PlayerCharacter::JAK,
                           .appearance = get_default_player_appearance(0x112233)};
  result.name[0] = 'J';
  result.name[1] = 'a';
  result.name[2] = 'k';
  return result;
}
}  // namespace

TEST(Jak2Replication, IdentityUsesSharedAppearanceValidation) {
  ReplicationState state;
  auto valid = identity(1);
  ASSERT_TRUE(state.update_player_identity(valid));
  EXPECT_TRUE(state.players().identities()[1].joined);

  auto invalid = identity(2);
  invalid.appearance.colors[5] = 0xff000000u;
  EXPECT_FALSE(state.update_player_identity(invalid));
  EXPECT_FALSE(state.players().identities()[2].joined);
}

TEST(Jak2Replication, DepartureClearsAllGameplayDomains) {
  ReplicationState state;
  PlayerState player = {.player_id = 1, .state_ready = true};
  ASSERT_TRUE(state.players().apply(player, from(1, 1)));
  EnemySnapshot enemies = {.source_player_id = 1,
                           .enemies = {{.actor_id = 41, .owner_player_id = 1}}};
  ASSERT_TRUE(state.entities().apply(enemies, from(1, 1)));
  GameEventBatch events = {.events = {{.event_id = 2, .source_player_id = 1}}};
  ASSERT_TRUE(state.events().apply(events, from(1, 1)));
  ASSERT_TRUE(state.depart_player(1));
  EXPECT_EQ(state.players().players()[1].last_sequence, 0u);
  EXPECT_TRUE(state.entities().enemies().enemies.empty());
  EXPECT_TRUE(state.events().events().empty());
}

TEST(Jak2Replication, WorldAndBootstrapPreserveAuthoritativeInventory) {
  ReplicationState state;
  BootstrapState bootstrap = {};
  bootstrap.world.money = 10;
  bootstrap.world.gems = 20;
  bootstrap.world.skill = 30;
  bootstrap.world.clock = 40;
  ASSERT_TRUE(state.apply_bootstrap(bootstrap, 2));
  WorldState world = {.money = 999, .gems = 999, .skill = 999, .clock = 50};
  ASSERT_TRUE(state.world().apply(world, {.sequence = 3, .source = {.from_host = true}}));
  EXPECT_FLOAT_EQ(state.world().world().money, 10);
  EXPECT_FLOAT_EQ(state.world().world().gems, 20);
  EXPECT_FLOAT_EQ(state.world().world().skill, 30);
  EXPECT_EQ(state.world().world().clock, 50u);
  EXPECT_FALSE(state.world().apply(world, {.sequence = 2, .source = {.from_host = true}}));
}

TEST(Jak2Replication, InvalidBootstrapDoesNotPartiallyMutateWorld) {
  ReplicationState state;
  BootstrapState bootstrap = {};
  bootstrap.world.money = 7;
  ASSERT_TRUE(state.apply_bootstrap(bootstrap, 1));
  bootstrap.world.money = 99;
  bootstrap.host_spawn_angle = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(state.apply_bootstrap(bootstrap, 2));
  EXPECT_FLOAT_EQ(state.world().world().money, 7);
}

TEST(Jak2Replication, EventsSequencePerSourceAndDrainWithinBounds) {
  ReplicationState state;
  GameEventBatch first = {
      .events = {{.event_id = 1, .source_player_id = 1}, {.event_id = 2, .source_player_id = 1}}};
  GameEventBatch second = {.events = {{.event_id = 3, .source_player_id = 2}}};
  ASSERT_TRUE(state.events().apply(first, from(1, 5)));
  ASSERT_TRUE(state.events().apply(second, from(2, 1)));
  EXPECT_FALSE(state.events().apply(first, from(1, 5)));
  const auto taken = state.events().take(2);
  ASSERT_EQ(taken.size(), 2u);
  EXPECT_EQ(taken[0].event_id, 1);
  EXPECT_EQ(taken[1].event_id, 2);
  EXPECT_EQ(state.events().events().size(), 1u);
}

TEST(Jak2Replication, EventsRejectInvalidBoundsBeforeMutation) {
  ReplicationState state;
  GameEvent invalid = {.event_id = 1, .source_player_id = 1, .payload_size = 65};
  EXPECT_FALSE(state.events().apply(GameEventBatch{.events = {invalid}}, from(1, 1)));
  GameEvent valid = {.event_id = 1, .source_player_id = 1};
  GameEventBatch oversized;
  oversized.events.assign(256, valid);
  EXPECT_FALSE(state.events().apply(oversized, from(1, 1)));
  EXPECT_TRUE(state.events().events().empty());
}

TEST(Jak2Replication, PlayerExpiryClearsDependentVehicleAndTurretState) {
  ReplicationState state;
  PlayerState player = {.player_id = 1,
                        .state_ready = true,
                        .turret_active = true,
                        .vehicle_id = kPlayerVehicleNetIdClass | 1u};
  ASSERT_TRUE(state.players().apply(player, from(1, 1, 1000)));
  PlayerVehicleState vehicle = {
      .player_id = 1,
      .vehicle = {.net_id = player.vehicle_id, .quaternion = {0.0f, 0.0f, 0.0f, 1.0f}}};
  ASSERT_TRUE(state.players().apply(vehicle, from(1, 1, 1000)));
  TurretState turret = {.player_id = 1, .turret_aid = player.vehicle_id};
  ASSERT_TRUE(state.players().apply(turret, from(1, 1, 1000)));
  state.expire(3001);
  EXPECT_FALSE(state.players().players()[1].state_ready);
  EXPECT_EQ(state.players().player_vehicles()[1].vehicle.net_id, 0u);
  EXPECT_EQ(state.players().turrets()[1].turret_aid, 0u);
}

TEST(Jak2Replication, PlayerExpiryUsesFullLocalClock) {
  ReplicationState state;
  const uint64_t received_at_ms = (uint64_t{1} << 32) + 1000;
  PlayerState player = {.player_id = 1, .state_ready = true};
  ASSERT_TRUE(state.players().apply(player, from(1, 1, received_at_ms)));
  EXPECT_EQ(state.players().players()[1].received_time_ms, received_at_ms);
  state.expire(received_at_ms + 2000);
  EXPECT_TRUE(state.players().players()[1].state_ready);
  state.expire(received_at_ms + 2001);
  EXPECT_FALSE(state.players().players()[1].state_ready);
}

TEST(Jak2Replication, BossAndAirlockValidationPrecedesMutation) {
  ReplicationState state;
  BossState boss = {.active = 1};
  boss.quaternion[3] = 1.0f;
  boss.root_quaternion[3] = 1.0f;
  ASSERT_TRUE(state.entities().apply(boss, from(1, 1, 1000)));
  boss.position[0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(state.entities().apply(boss, from(1, 2, 1500)));
  EXPECT_EQ(state.entities().boss(BossState::Kind::PALACE_SQUID).active, 1u);

  AirlockSnapshot airlocks = {.source_player_id = 1,
                              .states = {{.airlock_aid = 17, .state_id = 2}}};
  ASSERT_TRUE(state.entities().apply(airlocks, from(1, 8, 1000, 2)));
  AirlockSnapshot second_peer_airlocks = {.source_player_id = 2,
                                          .states = {{.airlock_aid = 18, .state_id = 1}}};
  ASSERT_TRUE(state.entities().apply(second_peer_airlocks, from(2, 1, 1100, 0)));
  airlocks.states[0].state_id = 9;
  EXPECT_FALSE(state.entities().apply(airlocks, from(1, 9, 1500, 2)));
  EXPECT_EQ(state.entities().airlocks()[1].states[0].state_id, 2u);
  EXPECT_EQ(state.entities().airlocks()[2].states[0].airlock_aid, 18u);
}

TEST(Jak2Replication, ResetClearsAllComposedState) {
  ReplicationState state;
  PlayerState player = {.player_id = 1};
  ASSERT_TRUE(state.players().apply(player, from(1, 1)));
  WorldState world = {.clock = 9};
  ASSERT_TRUE(state.world().apply(world, {.sequence = 1, .source = {.from_host = true}}));
  state.reset();
  EXPECT_EQ(state.players().players()[1].last_sequence, 0u);
  EXPECT_EQ(state.world().world().clock, 0u);
  EXPECT_TRUE(state.events().events().empty());
}
