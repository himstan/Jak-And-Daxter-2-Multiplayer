#include "game/multiplayer/jak2/core/replication_state.h"
#include "gtest/gtest.h"

namespace {
using namespace multiplayer::jak2::core;

ApplyContext from(const PlayerId player_id, const Sequence sequence, const uint64_t now_ms = 0) {
  return {.sequence = sequence,
          .source = {.authenticated_player_id = player_id},
          .received_at_ms = now_ms};
}

VehicleState player_vehicle(const uint32_t net_id, const float x) {
  VehicleState vehicle = {};
  vehicle.net_id = net_id;
  vehicle.position = {x, 2.0f, 3.0f};
  vehicle.quaternion[3] = 1.0f;
  return vehicle;
}
}  // namespace

TEST(Jak2Replication, SameVehicleSeatChangeUpdatesDetailedVehicle) {
  ReplicationState state;
  PlayerState driver = {};
  driver.player_id = 1;
  driver.vehicle_id = kPlayerVehicleNetIdClass | 1u;
  ASSERT_TRUE(state.players().apply(driver, from(1, 1)));

  PlayerVehicleState detail = {};
  detail.player_id = 1;
  detail.vehicle = player_vehicle(driver.vehicle_id, 10.0f);
  ASSERT_TRUE(state.players().apply(detail, from(1, 2)));

  driver.vehicle_seat = 1;
  ASSERT_TRUE(state.players().apply(driver, from(1, 3)));
  EXPECT_EQ(state.players().players()[1].vehicle_seat, 1u);
  EXPECT_EQ(state.players().player_vehicles()[1].seat_index, 1u);
}

TEST(Jak2Replication, LateTurretStateCannotResurrectAfterVehicleDismount) {
  ReplicationState state;
  PlayerState riding = {};
  riding.player_id = 1;
  riding.vehicle_id = kPlayerVehicleNetIdClass | 7u;
  riding.turret_active = true;
  ASSERT_TRUE(state.players().apply(riding, from(1, 1)));

  TurretState turret = {.player_id = 1, .turret_aid = riding.vehicle_id, .rotation_x = 0.25f};
  ASSERT_TRUE(state.players().apply(turret, from(1, 2)));
  riding.vehicle_id = 0;
  riding.turret_active = false;
  ASSERT_TRUE(state.players().apply(riding, from(1, 3)));

  turret.rotation_x = 0.75f;
  EXPECT_FALSE(state.players().apply(turret, from(1, 4)));
  EXPECT_EQ(state.players().turrets()[1].turret_aid, 0u);
  EXPECT_FALSE(state.players().players()[1].turret_active);
}

TEST(Jak2Replication, VehicleExitImmediatelyReleasesDriverStateAndRejectsLateUpdates) {
  PlayerReplicationState state;
  PlayerState driver = {.player_id = 1, .vehicle_id = 0x21000001u};
  ASSERT_TRUE(state.apply(driver, from(1, 1, 1000)));
  PlayerVehicleState detail = {.player_id = 1, .vehicle = player_vehicle(driver.vehicle_id, 10)};
  ASSERT_TRUE(state.apply(detail, from(1, 1, 1000)));
  driver.vehicle_id = 0;
  ASSERT_TRUE(state.apply(driver, from(1, 2, 1100)));
  EXPECT_EQ(state.player_vehicles()[1].vehicle.net_id, 0u);
  EXPECT_FALSE(state.apply(detail, from(1, 2, 1101)));
  EXPECT_EQ(state.players()[1].vehicle_id, 0u);
}

TEST(Jak2Replication, ReleasedVehicleKeepsMovingUpdatesForExactlyTwoSecondsWithoutReseatingDriver) {
  PlayerReplicationState state;
  PlayerState driver = {.player_id = 1, .vehicle_id = 0x21000001u};
  driver.selected_traffic_authority = 0;
  ASSERT_TRUE(state.apply(driver, from(1, 1, 1000)));
  PlayerVehicleState detail = {.player_id = 1, .vehicle = player_vehicle(driver.vehicle_id, 10)};
  ASSERT_TRUE(state.apply(detail, from(1, 1, 1000)));
  driver.vehicle_id = 0;
  ASSERT_TRUE(state.apply(driver, from(1, 2, 1100)));
  ASSERT_TRUE(state.apply(driver, from(1, 3, 2000)));
  detail.vehicle.position[0] = 30;
  detail.vehicle.linear_velocity[0] = 5;
  ASSERT_TRUE(state.apply(detail, from(1, 2, 3099)));
  EXPECT_EQ(state.players()[1].vehicle_id, 0u);
  EXPECT_FLOAT_EQ(state.player_vehicles()[1].vehicle.position[0], 30);
  EXPECT_FLOAT_EQ(state.player_vehicles()[1].vehicle.linear_velocity[0], 5);
  EXPECT_TRUE(state.owns_vehicle(1, detail.vehicle.net_id, 3099));
  EXPECT_FALSE(state.owns_vehicle(1, detail.vehicle.net_id, 3100));
  EXPECT_FALSE(state.apply(detail, from(1, 3, 3100)));
  state.expire(3100);
  EXPECT_EQ(state.player_vehicles()[1].vehicle.net_id, 0u);
}

TEST(Jak2Replication, NewDriverImmediatelyRevokesReleaseGraceButPassengerDoesNot) {
  PlayerReplicationState state;
  PlayerState driver = {.player_id = 1, .vehicle_id = 0x21000001u};
  driver.selected_traffic_authority = 0;
  ASSERT_TRUE(state.apply(driver, from(1, 1, 1000)));
  PlayerVehicleState detail = {.player_id = 1, .vehicle = player_vehicle(driver.vehicle_id, 10)};
  ASSERT_TRUE(state.apply(detail, from(1, 1, 1000)));
  driver.vehicle_id = 0;
  ASSERT_TRUE(state.apply(driver, from(1, 2, 1100)));
  PlayerState entrant = {.player_id = 2, .vehicle_id = detail.vehicle.net_id, .vehicle_seat = 1};
  ASSERT_TRUE(state.apply(entrant, from(2, 1, 1200)));
  EXPECT_TRUE(state.owns_vehicle(1, entrant.vehicle_id, 1200));
  entrant.vehicle_seat = 0;
  ASSERT_TRUE(state.apply(entrant, from(2, 2, 1300)));
  EXPECT_FALSE(state.owns_vehicle(1, entrant.vehicle_id, 1300));
  EXPECT_EQ(state.player_vehicles()[1].vehicle.net_id, 0u);
  EXPECT_FALSE(state.apply(detail, from(1, 2, 1301)));
  detail.player_id = 2;
  detail.vehicle.position[0] = 50;
  ASSERT_TRUE(state.apply(detail, from(2, 1, 1301)));
  EXPECT_FLOAT_EQ(state.player_vehicles()[2].vehicle.position[0], 50);
}

TEST(Jak2Replication, DelayedOldDriverExitCannotStartGraceAfterNewDriverClaimsVehicle) {
  PlayerReplicationState state;
  PlayerState driver = {.player_id = 1, .vehicle_id = 0x21000001u};
  driver.selected_traffic_authority = 0;
  ASSERT_TRUE(state.apply(driver, from(1, 1, 1000)));
  PlayerVehicleState detail = {.player_id = 1, .vehicle = player_vehicle(driver.vehicle_id, 10)};
  ASSERT_TRUE(state.apply(detail, from(1, 1, 1000)));
  ASSERT_TRUE(
      state.apply(PlayerState{.player_id = 2, .vehicle_id = driver.vehicle_id}, from(2, 1, 1100)));
  driver.vehicle_id = 0;
  ASSERT_TRUE(state.apply(driver, from(1, 2, 1200)));
  EXPECT_FALSE(state.owns_vehicle(1, detail.vehicle.net_id, 1201));
  EXPECT_FALSE(state.apply(detail, from(1, 2, 1201)));
}

TEST(Jak2Replication, RootDriversAndPassengersDoNotRetainReleaseGrace) {
  for (const bool passenger : {false, true}) {
    PlayerReplicationState state;
    PlayerState driver = {.player_id = 1, .vehicle_id = 0x21000001u};
    driver.vehicle_seat = passenger ? 1 : 0;
    driver.selected_traffic_authority = passenger ? 0 : 1;
    ASSERT_TRUE(state.apply(driver, from(1, 1, 1000)));
    PlayerVehicleState detail = {.player_id = 1, .vehicle = player_vehicle(driver.vehicle_id, 10)};
    ASSERT_TRUE(state.apply(detail, from(1, 1, 1000)));
    driver.vehicle_id = 0;
    ASSERT_TRUE(state.apply(driver, from(1, 2, 1100)));
    EXPECT_FALSE(state.owns_vehicle(1, detail.vehicle.net_id, 1101));
    EXPECT_FALSE(state.apply(detail, from(1, 2, 1101)));
  }
}

TEST(Jak2Replication, ReleasedVehicleAuthorityIsClearedByDepartureAndReset) {
  for (const bool reset : {false, true}) {
    PlayerReplicationState state;
    PlayerState driver = {.player_id = 1, .vehicle_id = 0x21000001u};
    driver.selected_traffic_authority = 0;
    ASSERT_TRUE(state.apply(driver, from(1, 1, 1000)));
    PlayerVehicleState detail = {.player_id = 1, .vehicle = player_vehicle(driver.vehicle_id, 10)};
    ASSERT_TRUE(state.apply(detail, from(1, 1, 1000)));
    driver.vehicle_id = 0;
    ASSERT_TRUE(state.apply(driver, from(1, 2, 1100)));
    if (reset)
      state.reset();
    else
      state.depart(1);
    EXPECT_FALSE(state.owns_vehicle(1, detail.vehicle.net_id, 1101));
  }
}

TEST(Jak2Replication, ReleasedVehicleReturnsToNormalTrafficWhenItsPlayerBecomesRoot) {
  PlayerReplicationState state;
  PlayerState driver = {.player_id = 1, .vehicle_id = 0x21000001u};
  driver.selected_traffic_authority = 0;
  ASSERT_TRUE(state.apply(driver, from(1, 1, 1000)));
  PlayerVehicleState detail = {.player_id = 1, .vehicle = player_vehicle(driver.vehicle_id, 10)};
  ASSERT_TRUE(state.apply(detail, from(1, 1, 1000)));
  driver.vehicle_id = 0;
  ASSERT_TRUE(state.apply(driver, from(1, 2, 1100)));
  driver.selected_traffic_authority = 1;
  ASSERT_TRUE(state.apply(driver, from(1, 3, 1200)));
  EXPECT_FALSE(state.owns_vehicle(1, detail.vehicle.net_id, 1201));
  EXPECT_EQ(state.player_vehicles()[1].vehicle.net_id, 0u);
  EXPECT_FALSE(state.apply(detail, from(1, 2, 1201)));
}

TEST(Jak2Replication, VehicleExitAcceptsFreshRootTrafficAfterDiscardingOldAmbientState) {
  ReplicationState state;
  TrafficAuthority authority = {.revision = 1};
  authority.assignments.fill(kInvalidPlayerId);
  authority.assignments[0] = 0;
  ASSERT_TRUE(state.traffic().apply(authority, {.source = {.from_host = true}}));
  ASSERT_TRUE(state.traffic().select_authority(0));
  TrafficSnapshot traffic = {.kind = TrafficSnapshot::Kind::VEHICLES,
                             .source_player_id = 0,
                             .authority_revision = 1,
                             .vehicles = {player_vehicle(0x21000001u, 1)}};
  ASSERT_TRUE(state.traffic().apply(traffic, from(0, 1, 1000)));
  PlayerState driver = {.player_id = 1, .vehicle_id = 0x21000001u};
  ASSERT_TRUE(state.players().apply(driver, from(1, 1, 1000)));
  PlayerVehicleState detail = {.player_id = 1, .vehicle = player_vehicle(driver.vehicle_id, 100)};
  ASSERT_TRUE(state.players().apply(detail, from(1, 1, 1000)));
  state.expire(1001);
  EXPECT_TRUE(state.traffic().selected_snapshot().vehicles.empty());
  driver.vehicle_id = 0;
  ASSERT_TRUE(state.players().apply(driver, from(1, 2, 1100)));
  EXPECT_FALSE(state.traffic().apply(traffic, from(0, 1, 1101)));
  traffic.vehicles[0].position[0] = 101;
  ASSERT_TRUE(state.traffic().apply(traffic, from(0, 2, 1101)));
  state.expire(1101);
  ASSERT_EQ(state.traffic().selected_snapshot().vehicles.size(), 1u);
  EXPECT_FLOAT_EQ(state.traffic().selected_snapshot().vehicles[0].position[0], 101);
}

TEST(Jak2Replication, TrafficExpiryDiscardsAllDriverSnapshotsButKeepsPassengers) {
  ReplicationState state;
  TrafficAuthority authority = {.revision = 1};
  authority.assignments.fill(kInvalidPlayerId);
  authority.assignments[0] = 0;
  ASSERT_TRUE(state.traffic().apply(authority, {.source = {.from_host = true}}));
  ASSERT_TRUE(state.traffic().select_authority(0));
  TrafficSnapshot traffic = {
      .kind = TrafficSnapshot::Kind::VEHICLES,
      .source_player_id = 0,
      .authority_revision = 1,
      .vehicles = {player_vehicle(0x21000001u, 1), player_vehicle(0x21000002u, 2),
                   player_vehicle(0x21000003u, 3)}};
  ASSERT_TRUE(state.traffic().apply(traffic, from(0, 1, 1000)));
  for (PlayerId player_id = 1; player_id <= 3; ++player_id) {
    PlayerVehicleState detail = {.player_id = player_id,
                                 .seat_index = static_cast<uint8_t>(player_id == 3),
                                 .vehicle = traffic.vehicles[player_id - 1]};
    ASSERT_TRUE(state.players().apply(detail, from(player_id, 1, 1000)));
  }
  state.expire(1001);
  ASSERT_EQ(state.traffic().aggregate().vehicles.size(), 1u);
  ASSERT_EQ(state.traffic().selected_snapshot().vehicles.size(), 1u);
  EXPECT_EQ(state.traffic().selected_snapshot().vehicles[0].net_id, 0x21000003u);
  EXPECT_FALSE(state.traffic().apply(traffic, from(0, 1, 1002)));
}
