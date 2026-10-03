#include <array>
#include <limits>
#include <span>
#include <vector>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/core/validation.h"
#include "game/multiplayer/jak2/wire/packets/airlock_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/bootstrap_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/boss_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/enemy_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/game_event_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/pedestrian_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/player_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/player_vehicle_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/vehicle_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/world_state_packet.h"
#include "gtest/gtest.h"
#include "test/multiplayer/jak2/unit/packet_test_helpers.h"

namespace mp_test = multiplayer::jak2::test;

namespace {

template <typename PacketT, typename CoreT>
bool encode_core(const CoreT& core, std::vector<uint8_t>& bytes, PacketT (*convert)(const CoreT&)) {
  return multiplayer::platform::wire::encode_packet(convert(core), bytes);
}

template <typename PacketT, typename CoreT>
bool decode_core(const std::span<const uint8_t> bytes, CoreT& core) {
  const auto packet = multiplayer::platform::wire::decode_packet<PacketT>(bytes);
  if (!packet) {
    return false;
  }
  multiplayer::jak2::wire::from_packet(*packet, core);
  return true;
}

}  // namespace

TEST(Jak2Protocol, MeasureSizeMatchesEmittedSizeForFixedWidthPacket) {
  multiplayer::jak2::wire::PlayerStatePacket packet = {};
  packet.sample_time_ms = 123;
  packet.position = {1.0f, -2.0f, 3.0f};
  packet.angle = 0.5f;
  packet.velocity = {4.0f, 5.0f, 6.0f};
  packet.levels[0] = {.level_id = 0x44, .flags = 0x01};
  packet.levels[1] = {.level_id = 0x33, .flags = 0x02};

  serialize::MeasureStream measure;
  ASSERT_TRUE(multiplayer::jak2::wire::serialize_fields(measure, packet));
  const auto encoded = multiplayer::platform::wire::encode_packet(packet);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_GE(measure.GetBitsProcessed(), static_cast<int64_t>(encoded->size() * 8));
  EXPECT_GE(measure.GetBytesProcessed(), static_cast<int64_t>(encoded->size()));
}

TEST(Jak2Protocol, RacingVelocitiesRoundTripInPlayerAndVehicleSnapshots) {
  using namespace multiplayer;
  for (const std::array<float, 3> velocity :
       {std::array<float, 3>{-17566.875f, -49152.598f, 263952.88f},
        std::array<float, 3>{409600.0f, -409600.0f, 0.0f},
        std::array<float, 3>{-524288.0f, 0.0f, 524288.0f}}) {
    jak2::wire::PlayerStatePacket player;
    player.velocity = velocity;
    jak2::wire::PlayerVehicleStatePacket vehicle;
    vehicle.vehicle.linear_velocity = velocity;
    jak2::wire::VehicleStateBatchPacket traffic;
    traffic.vehicles.push_back({.net_id = 1, .linear_velocity = velocity});
    const auto player_bytes = platform::wire::encode_packet(player);
    const auto vehicle_bytes = platform::wire::encode_packet(vehicle);
    const auto traffic_bytes = platform::wire::encode_packet(traffic);
    ASSERT_TRUE(player_bytes);
    ASSERT_TRUE(vehicle_bytes);
    ASSERT_TRUE(traffic_bytes);
    EXPECT_EQ(player_bytes->size(), jak2::wire::PlayerStatePacket::kPolicy.maximum_payload_bytes);
    EXPECT_EQ(vehicle_bytes->size(), jak2::wire::kPlayerVehicleStatePacketWireSize);
    EXPECT_EQ(traffic_bytes->size(), jak2::wire::kVehicleStateBatchPacketPrefixWireSize +
                                         (jak2::wire::kVehicleStateRecordWireBits + 7) / 8);
    const auto decoded_player =
        platform::wire::decode_packet<jak2::wire::PlayerStatePacket>(*player_bytes);
    const auto decoded_vehicle =
        platform::wire::decode_packet<jak2::wire::PlayerVehicleStatePacket>(*vehicle_bytes);
    const auto decoded_traffic =
        platform::wire::decode_packet<jak2::wire::VehicleStateBatchPacket>(*traffic_bytes);
    ASSERT_TRUE(decoded_player);
    ASSERT_TRUE(decoded_vehicle);
    ASSERT_TRUE(decoded_traffic);
    ASSERT_EQ(decoded_traffic->vehicles.size(), 1u);
    for (size_t axis = 0; axis < velocity.size(); ++axis) {
      EXPECT_NEAR(decoded_player->velocity[axis], velocity[axis],
                  platform::wire::kLinearVelocityResolution / 2.0f);
      EXPECT_NEAR(decoded_vehicle->vehicle.linear_velocity[axis], velocity[axis],
                  platform::wire::kLinearVelocityResolution / 2.0f);
      EXPECT_NEAR(decoded_traffic->vehicles[0].linear_velocity[axis], velocity[axis],
                  platform::wire::kLinearVelocityResolution / 2.0f);
    }
  }
}

TEST(Jak2Protocol, LinearVelocityBoundsRejectOutOfRangeAndNonfiniteSnapshots) {
  using namespace multiplayer;
  for (const float invalid :
       {platform::wire::kLinearVelocityMin - 1.0f, platform::wire::kLinearVelocityMax + 1.0f,
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
    for (size_t axis = 0; axis < 3; ++axis) {
      jak2::wire::PlayerStatePacket player;
      player.velocity[axis] = invalid;
      jak2::wire::PlayerVehicleStatePacket vehicle;
      vehicle.vehicle.linear_velocity[axis] = invalid;
      jak2::wire::VehicleStateBatchPacket traffic;
      traffic.vehicles.push_back({.net_id = 1});
      traffic.vehicles[0].linear_velocity[axis] = invalid;
      EXPECT_FALSE(platform::wire::encode_packet(player));
      EXPECT_FALSE(platform::wire::encode_packet(vehicle));
      EXPECT_FALSE(platform::wire::encode_packet(traffic));
    }
  }
}

TEST(Jak2Protocol, QuantizedFieldsStayWithinTheirDeclaredHalfStep) {
  multiplayer::jak2::wire::PlayerStatePacket player = {};
  player.position = {123456.0f, -654321.0f, 777777.0f};
  player.angle = 12345.0f;
  player.velocity = {1234.0f, -2345.0f, 3456.0f};
  const auto player_bytes = multiplayer::platform::wire::encode_packet(player);
  ASSERT_TRUE(player_bytes.has_value());
  const auto decoded_player =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::PlayerStatePacket>(
          *player_bytes);
  ASSERT_TRUE(decoded_player.has_value());
  for (size_t axis = 0; axis < player.position.size(); ++axis) {
    EXPECT_NEAR(decoded_player->position[axis], player.position[axis],
                multiplayer::platform::wire::kPositionResolution / 2.0f);
    EXPECT_NEAR(decoded_player->velocity[axis], player.velocity[axis],
                multiplayer::platform::wire::kLinearVelocityResolution / 2.0f);
  }
  EXPECT_NEAR(decoded_player->angle, player.angle,
              multiplayer::platform::wire::kAngleResolution / 2.0f);

  multiplayer::jak2::wire::PlayerVehicleStatePacket vehicle = {};
  vehicle.vehicle.position = {-123456.0f, 234567.0f, -345678.0f};
  vehicle.vehicle.quaternion = {0.321f, -0.456f, 0.777f, -0.111f};
  vehicle.vehicle.linear_velocity = {1234.0f, -2345.0f, 3456.0f};
  vehicle.vehicle.angular_velocity = {123.0f, -234.0f, 345.0f};
  const auto vehicle_bytes = multiplayer::platform::wire::encode_packet(vehicle);
  ASSERT_TRUE(vehicle_bytes.has_value());
  const auto decoded_vehicle =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::PlayerVehicleStatePacket>(
          *vehicle_bytes);
  ASSERT_TRUE(decoded_vehicle.has_value());
  for (size_t axis = 0; axis < vehicle.vehicle.position.size(); ++axis) {
    EXPECT_NEAR(decoded_vehicle->vehicle.position[axis], vehicle.vehicle.position[axis],
                multiplayer::platform::wire::kVehiclePositionResolution / 2.0f);
    EXPECT_NEAR(decoded_vehicle->vehicle.linear_velocity[axis],
                vehicle.vehicle.linear_velocity[axis],
                multiplayer::platform::wire::kLinearVelocityResolution / 2.0f);
    EXPECT_NEAR(decoded_vehicle->vehicle.angular_velocity[axis],
                vehicle.vehicle.angular_velocity[axis],
                multiplayer::platform::wire::kAngularVelocityResolution / 2.0f);
  }
  for (size_t component = 0; component < vehicle.vehicle.quaternion.size(); ++component) {
    EXPECT_FLOAT_EQ(decoded_vehicle->vehicle.quaternion[component],
                    vehicle.vehicle.quaternion[component]);
  }

  multiplayer::jak2::wire::WorldStatePacket world = {};
  world.time_of_day_ratio = 0.123f;
  world.weather_cloud = 0.456f;
  world.weather_fog = 0.777f;
  world.weather_rain = 0.999f;
  const auto world_bytes = multiplayer::platform::wire::encode_packet(world);
  ASSERT_TRUE(world_bytes.has_value());
  const auto decoded_world =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::WorldStatePacket>(
          *world_bytes);
  ASSERT_TRUE(decoded_world.has_value());
  EXPECT_NEAR(decoded_world->time_of_day_ratio, world.time_of_day_ratio,
              multiplayer::platform::wire::kUnitResolution / 2.0f);
  EXPECT_NEAR(decoded_world->weather_cloud, world.weather_cloud,
              multiplayer::platform::wire::kUnitResolution / 2.0f);
  EXPECT_NEAR(decoded_world->weather_fog, world.weather_fog,
              multiplayer::platform::wire::kUnitResolution / 2.0f);
  EXPECT_NEAR(decoded_world->weather_rain, world.weather_rain,
              multiplayer::platform::wire::kUnitResolution / 2.0f);

  multiplayer::jak2::wire::PalaceSquidStatePacket palace = {};
  palace.shield_hit_points = 0.371f;
  palace.trajectory_duration = 123.456f;
  const auto palace_bytes = multiplayer::platform::wire::encode_packet(palace);
  ASSERT_TRUE(palace_bytes.has_value());
  const auto decoded_palace =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::PalaceSquidStatePacket>(
          *palace_bytes);
  ASSERT_TRUE(decoded_palace.has_value());
  EXPECT_NEAR(decoded_palace->shield_hit_points, palace.shield_hit_points,
              multiplayer::platform::wire::kUnitResolution / 2.0f);
  EXPECT_NEAR(decoded_palace->trajectory_duration, palace.trajectory_duration,
              multiplayer::platform::wire::kTrajectoryDurationResolution / 2.0f);
}

TEST(Jak2Protocol, QuantizedOutboundValuesRejectNonFiniteAndOutOfRangeInputs) {
  multiplayer::jak2::wire::PlayerVehicleStatePacket vehicle = {};
  vehicle.vehicle.quaternion[0] = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(vehicle).has_value());
  vehicle.vehicle.quaternion[0] = 2.0f;
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(vehicle).has_value());

  multiplayer::jak2::wire::WorldStatePacket world = {};
  world.weather_fog = -0.01f;
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(world).has_value());

  multiplayer::jak2::wire::PalaceSquidStatePacket palace = {};
  palace.trajectory_duration = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(palace).has_value());
  palace.trajectory_duration = multiplayer::platform::wire::kTrajectoryDurationMax + 1.0f;
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(palace).has_value());
}

TEST(Jak2Protocol, GameEventRejectsTrailingBytesAndPreservesPayload) {
  multiplayer::jak2::core::GameEvent input = {};
  input.event_id = 1;
  input.source_player_id = 2;
  input.payload_size = 12;
  input.payload[0] = 0xaa;
  input.payload[1] = 0xbb;
  input.payload[2] = 0xcc;

  std::vector<uint8_t> bytes;
  const multiplayer::jak2::core::GameEventBatch input_batch = {.events = {input}};
  ASSERT_TRUE(encode_core<multiplayer::jak2::wire::GameEventBatchPacket>(
      input_batch, bytes, multiplayer::jak2::wire::to_packet));
  ASSERT_EQ(bytes.size(), 15u);

  multiplayer::jak2::core::GameEventBatch output_batch = {};
  ASSERT_TRUE(decode_core<multiplayer::jak2::wire::GameEventBatchPacket>(bytes, output_batch));
  ASSERT_EQ(output_batch.events.size(), 1u);
  const auto& output = output_batch.events.front();
  EXPECT_EQ(output.event_id, input.event_id);
  EXPECT_EQ(output.source_player_id, multiplayer::jak2::core::kInvalidPlayerId);
  EXPECT_EQ(output.payload_size, input.payload_size);
  EXPECT_EQ(output.payload[1], 0xbbu);

  bytes.push_back(0);
  EXPECT_FALSE(decode_core<multiplayer::jak2::wire::GameEventBatchPacket>(bytes, output_batch));
}

TEST(Jak2Protocol, WorldStateRoundTripsExactPacketSize) {
  multiplayer::jak2::core::WorldState input = {};
  input.clock = 1234;
  input.time_of_day_frame = 5678;
  input.time_of_day_ratio = 0.25f;
  input.weather_cloud = 0.5f;
  input.task_mask[4] = 1;
  input.active_task_mask[9] = 1;

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(encode_core<multiplayer::jak2::wire::WorldStatePacket>(
      input, bytes, multiplayer::jak2::wire::to_packet));
  ASSERT_EQ(bytes.size(), multiplayer::jak2::wire::kWorldStatePacketWireSize);

  multiplayer::jak2::core::WorldState output = {};
  ASSERT_TRUE(decode_core<multiplayer::jak2::wire::WorldStatePacket>(bytes, output));
  EXPECT_EQ(output.clock, input.clock);
  EXPECT_EQ(output.task_mask, input.task_mask);
  EXPECT_EQ(output.active_task_mask, input.active_task_mask);
  EXPECT_FALSE(decode_core<multiplayer::jak2::wire::WorldStatePacket>(
      std::span(bytes).first(bytes.size() - 1), output));
}

TEST(Jak2Protocol, EnemySnapshotRoundTripsExactCountedSize) {
  multiplayer::jak2::core::EnemySnapshot input = {};
  input.sample_time_ms = 42;
  multiplayer::jak2::core::EnemyState enemy = {};
  enemy.actor_id = 17;
  enemy.position = {1.0f, 2.0f, 3.0f};
  enemy.quaternion = {0.0f, 0.0f, 0.0f, 1.0f};
  enemy.owner_player_id = 1;
  enemy.focus_player_id = 2;
  enemy.attack_active = true;
  input.enemies.push_back(enemy);

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(encode_core<multiplayer::jak2::wire::EnemyStateBatchPacket>(
      input, bytes, multiplayer::jak2::wire::to_packet));
  EXPECT_EQ(bytes.size(), 29u);

  multiplayer::jak2::core::EnemySnapshot output = {};
  ASSERT_TRUE(decode_core<multiplayer::jak2::wire::EnemyStateBatchPacket>(bytes, output));
  ASSERT_EQ(output.enemies.size(), 1u);
  EXPECT_EQ(output.enemies[0].actor_id, 17u);
  EXPECT_TRUE(output.enemies[0].attack_active);
  EXPECT_EQ(output.enemies[0].owner_player_id, multiplayer::jak2::core::kInvalidPlayerId);
  EXPECT_FALSE(decode_core<multiplayer::jak2::wire::EnemyStateBatchPacket>(
      std::span(bytes).first(bytes.size() - 1), output));
}

TEST(Jak2Protocol, TrafficSnapshotsRoundTripThroughCoreModels) {
  multiplayer::jak2::core::TrafficSnapshot input = {};
  input.source_player_id = 1;
  input.authority_revision = 4;
  input.level_id = 0x1234;
  multiplayer::jak2::core::VehicleState vehicle = {};
  vehicle.net_id = 0x20008001;
  vehicle.position = {4.0f, 5.0f, 6.0f};
  vehicle.quaternion = {0.0f, 0.0f, 0.0f, 1.0f};
  input.vehicles.push_back(vehicle);

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(encode_core<multiplayer::jak2::wire::VehicleStateBatchPacket>(
      input, bytes, multiplayer::jak2::wire::to_vehicle_state_batch_packet));
  multiplayer::jak2::core::TrafficSnapshot output = {};
  ASSERT_TRUE(decode_core<multiplayer::jak2::wire::VehicleStateBatchPacket>(bytes, output));
  ASSERT_EQ(output.vehicles.size(), 1u);
  EXPECT_EQ(output.vehicles[0].net_id, vehicle.net_id);
  EXPECT_NEAR(output.vehicles[0].position[0], vehicle.position[0],
              multiplayer::platform::wire::kVehiclePositionResolution / 2.0f);
  EXPECT_EQ(output.vehicles[0].quaternion, vehicle.quaternion);
}

TEST(Jak2Protocol, GenericCodecCanonicalizesCoreSourceContext) {
  multiplayer::jak2::core::GameEvent event = {};
  event.event_id = 30;
  event.source_player_id = 2;
  event.payload_size = 1;
  event.payload[0] = 0x7f;
  const auto outbound = multiplayer::jak2::core::GameEventBatch{.events = {event}};

  const auto encoded =
      multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(outbound));
  ASSERT_TRUE(encoded);
  const auto decoded =
      mp_test::decode_events(*encoded, {.authenticated_player_id = 2, .from_host = true});
  ASSERT_TRUE(decoded);
  const auto& batch = *decoded;
  ASSERT_EQ(batch.events.size(), 1u);
  EXPECT_EQ(batch.events[0].source_player_id, 2);
}

TEST(Jak2Protocol, BossCodecsPreserveSenderTimeAndRejectWrongSizes) {
  for (const auto [kind, expected_size] :
       {std::pair{multiplayer::jak2::core::BossState::Kind::PALACE_SQUID, size_t{69}},
        std::pair{multiplayer::jak2::core::BossState::Kind::WIDOW, size_t{19}}}) {
    multiplayer::jak2::core::BossState input = {};
    input.kind = kind;
    input.active = 1;
    input.state_id = 4;
    input.sample_time_ms = UINT32_MAX - 3;
    input.target_player_id = multiplayer::jak2::core::kInvalidPlayerId;
    input.position = {1.0f, 2.0f, 3.0f};
    input.quaternion = {0.0f, 0.0f, 0.0f, 1.0f};
    input.root_position = {4.0f, 5.0f, 6.0f};
    input.root_quaternion = {0.0f, 0.0f, 0.0f, 1.0f};

    std::vector<uint8_t> bytes;
    if (kind == multiplayer::jak2::core::BossState::Kind::PALACE_SQUID) {
      ASSERT_TRUE(multiplayer::platform::wire::encode_packet(
          multiplayer::jak2::wire::to_palace_squid_state_packet(input), bytes));
      ASSERT_EQ(bytes.size(), expected_size);
      const auto packet = multiplayer::platform::wire::decode_packet<
          multiplayer::jak2::wire::PalaceSquidStatePacket>(bytes);
      ASSERT_TRUE(packet.has_value());
      multiplayer::jak2::core::BossState output = {};
      multiplayer::jak2::wire::from_packet(*packet, output);
      EXPECT_EQ(output.kind, kind);
      EXPECT_EQ(output.state_id, 4u);
      EXPECT_EQ(output.sample_time_ms, input.sample_time_ms);
      bytes.pop_back();
      EXPECT_FALSE(multiplayer::platform::wire::decode_packet<
                       multiplayer::jak2::wire::PalaceSquidStatePacket>(bytes)
                       .has_value());
    } else {
      ASSERT_TRUE(multiplayer::platform::wire::encode_packet(
          multiplayer::jak2::wire::to_widow_state_packet(input), bytes));
      ASSERT_EQ(bytes.size(), expected_size);
      const auto packet =
          multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::WidowStatePacket>(
              bytes);
      ASSERT_TRUE(packet.has_value());
      multiplayer::jak2::core::BossState output = {};
      multiplayer::jak2::wire::from_packet(*packet, output);
      EXPECT_EQ(output.kind, kind);
      EXPECT_EQ(output.state_id, 4u);
      EXPECT_EQ(output.sample_time_ms, input.sample_time_ms);
      bytes.pop_back();
      EXPECT_FALSE(
          multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::WidowStatePacket>(
              bytes)
              .has_value());
    }
  }
}

TEST(Jak2Protocol, BootstrapAndAirlockKeepFixedWireSizes) {
  multiplayer::jak2::core::BootstrapState bootstrap = {};
  bootstrap.world.money = 12.0f;
  bootstrap.world.clock = 44;
  bootstrap.world.task_mask[3] = 1;
  bootstrap.host_task = 7;
  bootstrap.host_spawn_position = {1.0f, 2.0f, 3.0f};
  bootstrap.host_camera_angle_y = 0.75f;
  bootstrap.synchronized_aid_count = 1;
  bootstrap.synchronized_aids[0] = 0x12345678;

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(multiplayer::platform::wire::encode_packet(
      multiplayer::jak2::wire::to_packet(bootstrap), bytes));
  ASSERT_EQ(bytes.size(), 214u);
  const auto bootstrap_packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::BootstrapStatePacket>(
          bytes);
  ASSERT_TRUE(bootstrap_packet.has_value());
  multiplayer::jak2::core::BootstrapState decoded_bootstrap = {};
  multiplayer::jak2::wire::from_packet(*bootstrap_packet, decoded_bootstrap);
  EXPECT_EQ(decoded_bootstrap.world.clock, bootstrap.world.clock);
  EXPECT_EQ(decoded_bootstrap.synchronized_aids[0], bootstrap.synchronized_aids[0]);
  EXPECT_FALSE(
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::BootstrapStatePacket>(
          std::span(bytes).first(bytes.size() - 1))
          .has_value());

  multiplayer::jak2::core::AirlockSnapshot airlocks = {};
  airlocks.states.push_back({.airlock_aid = 1, .state_id = 2, .level_id = 3, .sequence = 4});
  ASSERT_TRUE(multiplayer::platform::wire::encode_packet(
      multiplayer::jak2::wire::to_packet(airlocks), bytes));
  ASSERT_EQ(bytes.size(), 11u);
  const auto airlock_packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::AirlockStateBatchPacket>(
          bytes);
  ASSERT_TRUE(airlock_packet.has_value());
  multiplayer::jak2::core::AirlockSnapshot decoded_airlocks = {};
  multiplayer::jak2::wire::from_packet(*airlock_packet, decoded_airlocks);
  ASSERT_EQ(decoded_airlocks.states.size(), 1u);
  EXPECT_EQ(decoded_airlocks.states[0].airlock_aid, 1u);
  EXPECT_FALSE(
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::AirlockStateBatchPacket>(
          std::span(bytes).first(bytes.size() - 1))
          .has_value());
}

TEST(Jak2Protocol, BootstrapPreservesEveryPermanentAid) {
  multiplayer::jak2::core::BootstrapState bootstrap = {};
  bootstrap.synchronized_aid_count = multiplayer::jak2::core::kMaxBootstrapAids;
  for (size_t index = 0; index < bootstrap.synchronized_aid_count; ++index) {
    bootstrap.synchronized_aids[index] = 0x10000000u + static_cast<uint32_t>(index);
  }

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(multiplayer::platform::wire::encode_packet(
      multiplayer::jak2::wire::to_packet(bootstrap), bytes));
  EXPECT_LT(bytes.size(), 32768u);
  const auto packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::BootstrapStatePacket>(
          bytes);
  ASSERT_TRUE(packet.has_value());
  multiplayer::jak2::core::BootstrapState decoded = {};
  multiplayer::jak2::wire::from_packet(*packet, decoded);
  EXPECT_EQ(decoded.synchronized_aid_count, bootstrap.synchronized_aid_count);
  EXPECT_EQ(decoded.synchronized_aids[0], bootstrap.synchronized_aids[0]);
  EXPECT_EQ(decoded.synchronized_aids[128], bootstrap.synchronized_aids[128]);
  EXPECT_EQ(decoded.synchronized_aids.back(), bootstrap.synchronized_aids.back());

  auto oversized = multiplayer::jak2::wire::to_packet(bootstrap);
  oversized.synchronized_aids.push_back(0x20000000u);
  ++oversized.synchronized_aid_count;
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(oversized, bytes));
}

TEST(Jak2Protocol, EnemyAuthorityGrantNamesRecipientAndRejectsLegacyOrTruncatedBodies) {
  using namespace multiplayer::jak2;
  wire::GameEventBatchPacket packet;
  wire::GameEventRecord grant;
  grant.event_id = 10;
  grant.payload_size = 5;
  grant.payload[0] = 42;
  grant.payload[4] = 2;
  packet.events.push_back(grant);
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(multiplayer::platform::wire::encode_packet(packet, bytes));
  EXPECT_EQ(bytes.size(), 8u);
  const auto decoded =
      multiplayer::platform::wire::decode_packet<wire::GameEventBatchPacket>(bytes);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->events[0].payload[4], 2u);
  EXPECT_FALSE(multiplayer::platform::wire::decode_packet<wire::GameEventBatchPacket>(
      std::span(bytes).first(bytes.size() - 1)));
  packet.events[0].payload_size = 4;
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(packet, bytes));
}

TEST(Jak2Protocol, AmbientTrafficUsesFullNonzeroTwentyFourBitSequence) {
  using namespace multiplayer::jak2::core;
  EXPECT_TRUE(valid_traffic_vehicle_net_id(kTrafficVehicleNetIdClass | 0x10000u));
  EXPECT_TRUE(valid_traffic_vehicle_net_id(kTrafficVehicleNetIdClass | kTrafficNetIdSequenceMask));
  EXPECT_TRUE(valid_traffic_pedestrian_net_id(kTrafficPedestrianNetIdClass | 0x10000u, 0));
  EXPECT_FALSE(valid_traffic_vehicle_net_id(kTrafficVehicleNetIdClass));
}
