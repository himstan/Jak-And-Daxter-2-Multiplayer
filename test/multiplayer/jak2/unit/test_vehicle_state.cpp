#include "game/multiplayer/jak2/core/replication_state.h"
#include "gtest/gtest.h"

namespace {
using namespace multiplayer::jak2::core;

ApplyContext from(const PlayerId player_id, const Sequence sequence) {
  return {.sequence = sequence, .source = {.authenticated_player_id = player_id}};
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
