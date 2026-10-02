#include <array>
#include <limits>
#include <span>
#include <vector>

#include "game/multiplayer/jak2/core/replication_state.h"
#include "game/multiplayer/jak2/wire/event_types.h"
#include "game/multiplayer/jak2/wire/packets/airlock_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/bootstrap_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/enemy_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/gungame_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/pedestrian_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/player_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/traffic_authority_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/vehicle_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/world_state_packet.h"
#include "game/multiplayer/platform/core/message_policy.h"
#include "game/multiplayer/platform/wire/quantization.h"
#include "gtest/gtest.h"
#include "test/multiplayer/jak2/unit/packet_test_helpers.h"

namespace mp_test = multiplayer::jak2::test;

TEST(Jak2Protocol, GameplayBodyAndHostReencodeAreCanonical) {
  const std::vector<uint8_t> bytes = {1, 30, 1, 0x7f};
  const auto decoded = mp_test::decode_events(
      bytes, {.connection_id = 9, .authenticated_player_id = 0, .from_host = true});
  ASSERT_TRUE(decoded);
  const auto reencoded =
      multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(*decoded));
  ASSERT_TRUE(reencoded);
  EXPECT_EQ(*reencoded, bytes);
}

TEST(Jak2Protocol, EventEnvelopesPreserveEveryPayloadByte) {
  constexpr std::array<uint8_t, 61> sizes = {
      4,    4,  4,    63,   4, 64, 64, 64, 17, 5,  64, 0,  0, 62, 62, 0,  0,  32, 39, 39, 39,
      4,    55, 9,    62,   5, 20, 37, 46, 1,  53, 17, 37, 9, 17, 1,  32, 32, 0,  0,  52, 0xff,
      0xff, 8,  0xff, 0xff, 4, 4,  4,  4,  1,  28, 18, 0,  0, 0,  31, 0,  12, 4,  1};
  for (uint8_t id = 1; id <= sizes.size(); ++id) {
    SCOPED_TRACE(id);
    if (sizes[id - 1] == 0xff) {
      EXPECT_EQ(multiplayer::jak2::wire::event_descriptor(id), nullptr);
      continue;
    }
    std::vector<uint8_t> bytes = {1, id, sizes[id - 1]};
    for (uint8_t index = 0; index < sizes[id - 1]; ++index) {
      bytes.push_back(static_cast<uint8_t>(id + index * 3));
    }
    if (id == 10) {
      bytes[7] = 2;
    }
    const auto decoded = mp_test::decode_events(bytes, {.authenticated_player_id = 2});
    ASSERT_TRUE(decoded);
    const auto& batch = *decoded;
    ASSERT_EQ(batch.events.size(), 1u);
    const auto& event = batch.events.front();
    EXPECT_EQ(event.event_id, id);
    EXPECT_EQ(event.payload_size, sizes[id - 1]);
    EXPECT_TRUE(std::equal(bytes.begin() + 3, bytes.end(), event.payload.begin()));
    const auto encoded =
        multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(
            multiplayer::jak2::core::GameEventBatch{.events = {event}}));
    ASSERT_TRUE(encoded);
    EXPECT_EQ(*encoded, bytes);
    for (size_t length = 0; length < bytes.size(); ++length) {
      EXPECT_FALSE(mp_test::decode_events(std::span(bytes).first(length), {}));
    }
    bytes[0] = 2;
    EXPECT_FALSE(mp_test::decode_events(bytes, {}));
    bytes[0] = 1;
    bytes.push_back(0);
    EXPECT_FALSE(mp_test::decode_events(bytes, {}));
  }
}

namespace {
template <typename Model>
void expect_exact_packet(const Model& payload, const std::vector<uint8_t>& expected) {
  const auto encoded =
      multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(payload));
  ASSERT_TRUE(encoded);
  EXPECT_EQ(*encoded, expected);
  for (size_t size = 0; size < expected.size(); ++size) {
    SCOPED_TRACE(size);
    using Packet = decltype(multiplayer::jak2::wire::to_packet(payload));
    const auto truncated = multiplayer::platform::wire::decode_packet<Packet>(
        std::span<const uint8_t>(expected.data(), size));
    EXPECT_FALSE(truncated);
  }
  auto trailing = expected;
  trailing.push_back(0);
  using Packet = decltype(multiplayer::jak2::wire::to_packet(payload));
  EXPECT_FALSE(multiplayer::platform::wire::decode_packet<Packet>(trailing));
}
}  // namespace

TEST(Jak2Protocol, PlayerStateWireFormatPreservesEveryField) {
  const std::vector<uint8_t> bytes = {120, 86,  52, 18,  3,  0,   16,  0,   0,   1,   0,   16,  0,
                                      16,  0,   32, 0,   64, 1,   128, 98,  136, 2,   102, 12,  68,
                                      16,  34,  6,  170, 36, 204, 70,  5,   154, 87,  35,  220, 91,
                                      0,   160, 20, 0,   0,  160, 88,  106, 59,  183, 0};
  multiplayer::jak2::core::PlayerState state = {};
  state.player_id = 2;
  state.sample_time_ms = 0x12345678;
  state.activity = multiplayer::jak2::core::PlayerActivity::IN_GAME;
  state.position = {1.0f, -2.0f, 3.0f};
  state.angle = 0.5f;
  state.velocity = {4.0f, -8.0f, 16.0f};
  state.state_id = 0x31;
  state.levels[0] = {.level_id = 0x44, .flags = 0x01};
  state.levels[1] = {.level_id = 0x33, .flags = 0x06};
  state.levels[2] = {.level_id = 0x22, .flags = 0x08};
  state.levels[3] = {.level_id = 0x11, .flags = 0x03};
  state.levels[4] = {.level_id = 0x55, .flags = 0x12};
  state.levels[5] = {.level_id = 0x66, .flags = 0xa3};
  state.darkjak_stage = 2;
  state.buttons = 0xabcd;
  state.leftx = 0x11;
  state.lefty = 0xee;
  state.spectator_only = true;
  state.turret_active = true;
  state.respawn_flags = 5;
  state.camera_angle_y = -0.25f;
  state.vehicle_id = 0x40000029;
  state.vehicle_seat = 1;
  state.equipped_weapon = 4;
  state.action_sequence = 0xa5;
  state.action_state_id = 0xb6;
  state.riding_along_player_id = 3;
  state.mission_flags = 3;
  state.visual_secrets = 5;
  state.selected_traffic_authority = 5;
  expect_exact_packet(state, bytes);
  const auto decoded = mp_test::decode_player(bytes, {.authenticated_player_id = 2});
  ASSERT_TRUE(decoded);
  const auto& actual = *decoded;
  EXPECT_EQ(actual.player_id, 2u);
  EXPECT_EQ(actual.sample_time_ms, 0x12345678u);
  EXPECT_EQ(actual.activity, multiplayer::jak2::core::PlayerActivity::IN_GAME);
  for (size_t axis = 0; axis < state.position.size(); ++axis) {
    EXPECT_NEAR(actual.position[axis], state.position[axis],
                multiplayer::platform::wire::kPositionResolution / 2.0f);
    EXPECT_NEAR(actual.velocity[axis], state.velocity[axis],
                multiplayer::platform::wire::kLinearVelocityResolution / 2.0f);
  }
  EXPECT_NEAR(actual.angle, 0.5f, multiplayer::platform::wire::kAngleResolution / 2.0f);
  EXPECT_EQ(actual.state_id, 0x31u);
  for (size_t level = 0; level < state.levels.size(); ++level) {
    EXPECT_EQ(actual.levels[level].level_id, state.levels[level].level_id);
    EXPECT_EQ(actual.levels[level].flags, state.levels[level].flags);
  }
  EXPECT_EQ(actual.darkjak_stage, 2u);
  EXPECT_EQ(actual.buttons, 0xabcdu);
  EXPECT_EQ(actual.leftx, 0x11u);
  EXPECT_EQ(actual.lefty, 0xeeu);
  EXPECT_TRUE(actual.spectator_only);
  EXPECT_FALSE(actual.scene_active);
  EXPECT_TRUE(actual.turret_active);
  EXPECT_EQ(actual.respawn_flags, 5u);
  EXPECT_NEAR(actual.camera_angle_y, -0.25f, multiplayer::platform::wire::kAngleResolution / 2.0f);
  EXPECT_EQ(actual.vehicle_id, 0x40000029u);
  EXPECT_EQ(actual.vehicle_seat, 1u);
  EXPECT_EQ(actual.equipped_weapon, 4u);
  EXPECT_EQ(actual.action_sequence, 0xa5u);
  EXPECT_EQ(actual.action_state_id, 0xb6u);
  EXPECT_EQ(actual.riding_along_player_id, 3u);
  EXPECT_EQ(actual.selected_traffic_authority, 5u);
  EXPECT_EQ(actual.mission_flags, 3u);
  EXPECT_EQ(actual.visual_secrets, 5u);
}

TEST(Jak2Protocol, PlayerVehicleWireFormatPreservesCivilianAndVacantRiders) {
  const std::vector<uint8_t> bytes = {120, 86, 52, 18,  41, 0,   0,   64, 3,  4,   5,  0, 128, 0,
                                      0,   8,  0,  128, 0,  0,   0,   0,  0,  0,   0,  0, 0,   0,
                                      0,   0,  0,  0,   0,  127, 0,   0,  5,  0,   10, 0, 4,   0,
                                      4,   0,  4,  0,   36, 145, 105, 36, 32, 252, 1};
  multiplayer::jak2::core::PlayerVehicleState state = {};
  state.player_id = 2;
  auto& vehicle = state.vehicle;
  vehicle.sample_time_ms = 0x12345678;
  vehicle.net_id = 0x40000029;
  vehicle.vehicle_type = 3;
  vehicle.color_index = 4;
  vehicle.state_id = 5;
  vehicle.target_player_id = 0;
  vehicle.position = {1.0f, 2.0f, 3.0f};
  vehicle.quaternion[3] = 1.0f;
  vehicle.linear_velocity = {4.0f, 8.0f, 16.0f};
  vehicle.angular_velocity = {-1.0f, -2.0f, -4.0f};
  vehicle.state_flags = 0x12;
  vehicle.hit_points = 200;
  vehicle.level_id = 0x1234;
  vehicle.rider_player_ids = {0, 1, 0xfffffffe, 0xff};
  expect_exact_packet(state, bytes);
  const auto decoded = mp_test::decode_player_vehicle(bytes, {.authenticated_player_id = 2});
  ASSERT_TRUE(decoded);
  const auto& actual = *decoded;
  EXPECT_EQ(actual.player_id, 2u);
  EXPECT_EQ(actual.vehicle.sample_time_ms, 0x12345678u);
  EXPECT_EQ(actual.vehicle.net_id, vehicle.net_id);
  EXPECT_EQ(actual.vehicle.vehicle_type, 3u);
  EXPECT_EQ(actual.vehicle.color_index, 4u);
  EXPECT_EQ(actual.vehicle.state_id, 5u);
  EXPECT_EQ(actual.vehicle.target_player_id, 0u);
  for (size_t axis = 0; axis < vehicle.position.size(); ++axis) {
    EXPECT_NEAR(actual.vehicle.position[axis], vehicle.position[axis],
                multiplayer::platform::wire::kVehiclePositionResolution / 2.0f);
    EXPECT_NEAR(actual.vehicle.linear_velocity[axis], vehicle.linear_velocity[axis],
                multiplayer::platform::wire::kLinearVelocityResolution / 2.0f);
    EXPECT_NEAR(actual.vehicle.angular_velocity[axis], vehicle.angular_velocity[axis],
                multiplayer::platform::wire::kAngularVelocityResolution / 2.0f);
  }
  for (size_t component = 0; component < vehicle.quaternion.size(); ++component) {
    EXPECT_FLOAT_EQ(actual.vehicle.quaternion[component], vehicle.quaternion[component]);
  }
  EXPECT_EQ(actual.vehicle.state_flags, 0x12u);
  EXPECT_EQ(actual.vehicle.hit_points, 200u);
  EXPECT_EQ(actual.vehicle.level_id, 0x1234u);
  EXPECT_EQ(actual.vehicle.rider_player_ids, vehicle.rider_player_ids);
  multiplayer::jak2::core::ReplicationState replication;
  ASSERT_TRUE(replication.players().apply(
      *decoded, {.sequence = 0x01020304, .source = {.authenticated_player_id = 2}}));
  EXPECT_EQ(replication.players().player_vehicles()[2].vehicle.rider_player_ids,
            vehicle.rider_player_ids);
  auto malformed = bytes;
  malformed[50] = static_cast<uint8_t>((malformed[50] & ~0x18u) | 0x10u);
  EXPECT_FALSE(mp_test::decode_player_vehicle(malformed, {.authenticated_player_id = 2}));
}

TEST(Jak2Protocol, TurretWireFormatKeepsYawBeforePitch) {
  const std::vector<uint8_t> bytes = {0x29, 0, 0, 0x40, 0, 0x40, 0, 0x40};
  multiplayer::jak2::core::TurretState state = {};
  state.player_id = 2;
  state.turret_aid = 0x40000029;
  state.rotation_y = -0.5f;
  state.rotation_x = 0.25f;
  expect_exact_packet(state, bytes);
  const auto decoded = mp_test::decode_turret(bytes, {.authenticated_player_id = 2});
  ASSERT_TRUE(decoded);
  const auto& actual = *decoded;
  EXPECT_EQ(actual.player_id, 2u);
  EXPECT_EQ(actual.turret_aid, 0x40000029u);
  EXPECT_NEAR(actual.rotation_y, -0.5f, multiplayer::platform::wire::kAngleResolution / 2.0f);
  EXPECT_NEAR(actual.rotation_x, 0.25f, multiplayer::platform::wire::kAngleResolution / 2.0f);
}

TEST(Jak2Protocol, AuthorityWireFormatKeepsRevisionSeparateFromPacketSequence) {
  const std::vector<uint8_t> bytes = {0x44, 0x33, 0x22, 0x11, 0, 0x22, 0xff, 2};
  multiplayer::jak2::core::TrafficAuthority authority = {};
  authority.revision = 0x11223344;
  authority.assignments = {0, 0, 2, 2, 0xff, 0xff, 2, 0};
  expect_exact_packet(authority, bytes);
  const auto decoded =
      mp_test::decode_authority(bytes, {.authenticated_player_id = 0, .from_host = true});
  ASSERT_TRUE(decoded);
  const auto& actual = *decoded;
  EXPECT_EQ(actual.revision, 0x11223344u);
  EXPECT_EQ(actual.assignments, authority.assignments);
}

TEST(Jak2Protocol, TrafficAuthorityRoundTripsAllAssignments) {
  multiplayer::jak2::core::TrafficAuthority authority = {};
  authority.revision = 4;
  authority.assignments.fill(multiplayer::jak2::core::kInvalidPlayerId);
  authority.assignments[1] = 1;
  const auto encoded =
      multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(authority));
  ASSERT_TRUE(encoded);
  multiplayer::jak2::core::TrafficAuthority decoded = {};
  const auto packet = multiplayer::platform::wire::decode_packet<
      multiplayer::jak2::wire::TrafficAuthorityStatePacket>(*encoded);
  ASSERT_TRUE(packet);
  multiplayer::jak2::wire::from_packet(*packet, decoded);
  EXPECT_EQ(decoded.revision, 4u);
  EXPECT_EQ(decoded.assignments[1], 1u);
  EXPECT_EQ(decoded.assignments[2], multiplayer::jak2::core::kInvalidPlayerId);
}

TEST(Jak2Protocol, BootstrapRejectsTruncationAndTrailingBytes) {
  multiplayer::jak2::core::BootstrapState bootstrap = {};
  bootstrap.host_continue[0] = 'x';
  const auto encoded =
      multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(bootstrap));
  ASSERT_TRUE(encoded);
  EXPECT_FALSE(
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::BootstrapStatePacket>(
          std::span<const uint8_t>(encoded->data(), encoded->size() - 1)));
  std::vector<uint8_t> trailing = *encoded;
  trailing.push_back(0);
  EXPECT_FALSE(
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::BootstrapStatePacket>(
          trailing));
}

TEST(Jak2Protocol, AirlockUsesCountedWireSize) {
  multiplayer::jak2::core::AirlockSnapshot snapshot = {};
  snapshot.states.push_back({.airlock_aid = 12, .state_id = 3, .level_id = 4});
  const auto encoded =
      multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(snapshot));
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded->size(), 11u);
}

TEST(Jak2Protocol, AirlockPayloadOmitsSequenceAndRelaySource) {
  multiplayer::jak2::core::AirlockSnapshot snapshot = {};
  snapshot.source_player_id = 2;
  snapshot.sequence = 7;
  snapshot.states = {{.airlock_aid = 11, .state_id = 3}};
  const auto encoded =
      multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(snapshot));
  ASSERT_TRUE(encoded);
  EXPECT_EQ((*encoded)[0] & 0x07u, 1u);
  multiplayer::jak2::core::AirlockSnapshot decoded;
  const auto packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::AirlockStateBatchPacket>(
          *encoded);
  ASSERT_TRUE(packet);
  multiplayer::jak2::wire::from_packet(*packet, decoded);
  EXPECT_EQ(decoded.sequence, 0u);
  EXPECT_EQ(decoded.source_player_id, multiplayer::jak2::core::kInvalidPlayerId);
}

TEST(Jak2Protocol, EnemyBatchSupportsSchemaCapacityButNoMore) {
  multiplayer::jak2::core::EnemySnapshot snapshot = {};
  snapshot.source_player_id = 1;
  snapshot.enemies.resize(multiplayer::jak2::core::kMaxEnemies);
  for (uint32_t index = 0; index < snapshot.enemies.size(); ++index) {
    snapshot.enemies[index].actor_id = index + 1;
  }
  const auto encoded =
      multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(snapshot));
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded->size(),
            (&multiplayer::jak2::wire::EnemyStateBatchPacket::kPolicy)->maximum_payload_bytes);
  multiplayer::jak2::core::EnemySnapshot decoded = {};
  const auto packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::EnemyStateBatchPacket>(
          *encoded);
  ASSERT_TRUE(packet);
  multiplayer::jak2::wire::from_packet(*packet, decoded);
  EXPECT_EQ(decoded.enemies.size(), multiplayer::jak2::core::kMaxEnemies);

  std::vector<uint8_t> malformed = *encoded;
  malformed[4] = static_cast<uint8_t>(multiplayer::jak2::core::kMaxEnemies + 1);
  EXPECT_FALSE(
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::EnemyStateBatchPacket>(
          malformed));
  snapshot.enemies.push_back({.actor_id = 99});
  EXPECT_FALSE(
      multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(snapshot)));
}

TEST(Jak2Protocol, PedestrianAndVehiclePacketsRemainSeparate) {
  multiplayer::jak2::core::TrafficSnapshot pedestrians = {};
  pedestrians.kind = multiplayer::jak2::core::TrafficSnapshot::Kind::PEDESTRIANS;
  pedestrians.source_player_id = 1;
  pedestrians.pedestrians.push_back({.net_id = 0x10010001u});
  multiplayer::jak2::core::TrafficSnapshot vehicles = {};
  vehicles.kind = multiplayer::jak2::core::TrafficSnapshot::Kind::VEHICLES;
  vehicles.source_player_id = 1;
  vehicles.vehicles.push_back({.net_id = 0x20010001u});
  const auto pedestrian_bytes = multiplayer::platform::wire::encode_packet(
      multiplayer::jak2::wire::to_pedestrian_state_batch_packet(pedestrians));
  const auto vehicle_bytes = multiplayer::platform::wire::encode_packet(
      multiplayer::jak2::wire::to_vehicle_state_batch_packet(vehicles));
  ASSERT_TRUE(pedestrian_bytes);
  ASSERT_TRUE(vehicle_bytes);
  EXPECT_EQ(pedestrian_bytes->size(), 49u);
  EXPECT_EQ(vehicle_bytes->size(), 61u);
}

TEST(Jak2Protocol, EnemyDecodeInfersOwnerForHostRelay) {
  multiplayer::jak2::core::EnemySnapshot snapshot = {};
  snapshot.enemies.push_back({.actor_id = 7, .owner_player_id = 2});
  const auto encoded =
      multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(snapshot));
  ASSERT_TRUE(encoded);
  const auto decoded =
      mp_test::decode_enemies(*encoded, {.authenticated_player_id = 2, .from_host = true});
  ASSERT_TRUE(decoded);
  const auto& enemy = *decoded;
  EXPECT_EQ(enemy.source_player_id, 2u);
}

TEST(Jak2Protocol, PlayerActivityRoundTripsInTwoBitsWithoutChangingSize) {
  for (uint8_t value = 0; value <= 3; ++value) {
    SCOPED_TRACE(value);
    multiplayer::jak2::core::PlayerState state = {};
    state.player_id = 1;
    state.activity = static_cast<multiplayer::jak2::core::PlayerActivity>(value);
    const auto encoded =
        multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(state));
    ASSERT_TRUE(encoded);
    ASSERT_EQ(encoded->size(),
              (&multiplayer::jak2::wire::PlayerStatePacket::kPolicy)->maximum_payload_bytes);
    multiplayer::jak2::core::PlayerState decoded = {};
    const auto packet =
        multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::PlayerStatePacket>(
            *encoded);
    ASSERT_TRUE(packet);
    multiplayer::jak2::wire::from_packet(*packet, decoded);
    EXPECT_EQ(decoded.activity, state.activity);
  }

  multiplayer::jak2::wire::PlayerStatePacket invalid = {};
  invalid.activity = static_cast<multiplayer::jak2::core::PlayerActivity>(4);
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(invalid));
}

TEST(Jak2Protocol, QuantizationRejectsNonFiniteAndOutOfRangeValues) {
  multiplayer::jak2::wire::PlayerStatePacket packet = {};
  packet.position[0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(packet).has_value());
  packet.position[0] =
      multiplayer::platform::wire::kPositionMax + multiplayer::platform::wire::kPositionResolution;
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(packet).has_value());
}

TEST(Jak2Protocol, StripCoordinatesRoundTripAccurately) {
  multiplayer::jak2::wire::PlayerStatePacket packet = {};
  packet.activity = multiplayer::jak2::core::PlayerActivity::IN_GAME;
  packet.position = {11098202.0f, 469663.34f, 371692.34f};
  const auto encoded = multiplayer::platform::wire::encode_packet(packet);
  ASSERT_TRUE(encoded.has_value());
  ASSERT_EQ(encoded->size(),
            (&multiplayer::jak2::wire::PlayerStatePacket::kPolicy)->maximum_payload_bytes);

  const auto decoded =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::PlayerStatePacket>(
          *encoded);
  ASSERT_TRUE(decoded.has_value());
  for (size_t axis = 0; axis < packet.position.size(); ++axis) {
    EXPECT_NEAR(decoded->position[axis], packet.position[axis],
                multiplayer::platform::wire::kPositionResolution);
  }

  multiplayer::jak2::wire::BootstrapStatePacket bootstrap = {};
  bootstrap.host_spawn_position = packet.position;
  const auto encoded_bootstrap = multiplayer::platform::wire::encode_packet(bootstrap);
  ASSERT_TRUE(encoded_bootstrap.has_value());
  const auto decoded_bootstrap =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::BootstrapStatePacket>(
          *encoded_bootstrap);
  ASSERT_TRUE(decoded_bootstrap.has_value());
  for (size_t axis = 0; axis < packet.position.size(); ++axis) {
    EXPECT_NEAR(decoded_bootstrap->host_spawn_position[axis], packet.position[axis],
                multiplayer::platform::wire::kPositionResolution);
  }
}

TEST(Jak2Protocol, CompactReferenceCodesRejectReservedValues) {
  multiplayer::jak2::wire::PlayerStatePacket player = {};
  player.riding_along_player_id = 8;
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(player).has_value());

  multiplayer::jak2::wire::TrafficAuthorityStatePacket authority = {};
  authority.revision = 1;
  authority.assignments.fill(multiplayer::jak2::wire::kWireInvalidPlayerId);
  authority.assignments[0] = 8;
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(authority).has_value());

  multiplayer::jak2::wire::VehicleStateBatchPacket vehicles = {};
  multiplayer::jak2::wire::VehicleStateRecord vehicle = {};
  vehicle.net_id = 1;
  vehicle.rider_player_ids[0] = 8;
  vehicles.vehicles.push_back(vehicle);
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(vehicles).has_value());
}

TEST(Jak2Protocol, TypedDecodeRejectsEmptyPackets) {
  EXPECT_FALSE(
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::PlayerStatePacket>({}));
}

TEST(Jak2Protocol, WorldEndianFieldsRoundTripWithoutEnvelopeSequence) {
  multiplayer::jak2::core::WorldState state = {};
  state.clock = 123;
  const auto encoded =
      multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(state));
  ASSERT_TRUE(encoded);
  EXPECT_EQ((*encoded)[0], 123u);
  EXPECT_EQ((*encoded)[1], 0u);
  multiplayer::jak2::core::WorldState decoded = {};
  const auto packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::WorldStatePacket>(
          *encoded);
  ASSERT_TRUE(packet);
  multiplayer::jak2::wire::from_packet(*packet, decoded);
  EXPECT_EQ(decoded.clock, 123u);
}

TEST(Jak2Protocol, GungameStateUsesCompleteCompactTargetCollection) {
  using namespace multiplayer;
  jak2::core::GungameState state = {.run_id = 0x01020304,
                                    .score = -2,
                                    .elapsed_time = -120,
                                    .course_id = 3,
                                    .phase = jak2::core::GungamePhase::COURSE,
                                    .red_intro_step = 4,
                                    .yellow_intro_step = 3,
                                    .end_door = 1,
                                    .open_end = true};
  state.targets = {{.state = jak2::core::GungameTargetState::NOT_SPAWNED},
                   {.spawn_time = 300, .state = jak2::core::GungameTargetState::SPAWNED},
                   {.spawn_time = 600, .state = jak2::core::GungameTargetState::BROKEN}};
  auto bytes = platform::wire::encode_packet(jak2::wire::to_packet(state));
  ASSERT_TRUE(bytes);
  EXPECT_EQ(*bytes, (std::vector<uint8_t>{4,   3, 2, 1,  254, 255, 255, 255, 136, 255, 255,
                                          255, 3, 0, 3,  3,   4,   3,   1,   1,   36,  0,
                                          0,   0, 0, 44, 1,   0,   0,   88,  2,   0,   0}));
  for (const size_t count : {137u, 191u, 209u, 109u, 257u, 7705u}) {
    state.targets.resize(count, {.state = jak2::core::GungameTargetState::BROKEN});
    bytes = platform::wire::encode_packet(jak2::wire::to_packet(state));
    ASSERT_TRUE(bytes);
    EXPECT_EQ(bytes->size(), 20 + (count + 3) / 4 + count * 4);
    auto decoded = platform::wire::decode_packet<jak2::wire::GungameStatePacket>(*bytes);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->state.targets, state.targets);
    EXPECT_EQ(decoded->state.score, -2);
    EXPECT_EQ(decoded->state.elapsed_time, -120);
  }
  state.targets.resize(7706);
  EXPECT_FALSE(platform::wire::encode_packet(jak2::wire::to_packet(state)));
}

TEST(Jak2Protocol, GungameStateRejectsMalformedAndInconsistentBodies) {
  using namespace multiplayer;
  jak2::core::GungameState state = {
      .run_id = 1,
      .course_id = 1,
      .phase = jak2::core::GungamePhase::COURSE,
      .targets = {{.state = jak2::core::GungameTargetState::SPAWNED}}};
  const auto encoded = platform::wire::encode_packet(jak2::wire::to_packet(state));
  ASSERT_TRUE(encoded);
  for (size_t size = 0; size < encoded->size(); ++size)
    EXPECT_FALSE(platform::wire::decode_packet<jak2::wire::GungameStatePacket>(
        std::span(*encoded).first(size)));
  for (const auto [offset, value] : std::vector<std::pair<size_t, uint8_t>>{
           {0, 0}, {14, 5}, {15, 5}, {16, 5}, {17, 4}, {18, 2}, {19, 2}, {20, 3}}) {
    auto malformed = *encoded;
    malformed[offset] = value;
    EXPECT_FALSE(platform::wire::decode_packet<jak2::wire::GungameStatePacket>(malformed));
  }
  auto trailing = *encoded;
  trailing.push_back(0);
  EXPECT_FALSE(platform::wire::decode_packet<jak2::wire::GungameStatePacket>(trailing));
  auto oversized_count = *encoded;
  oversized_count[12] = 0xff;
  oversized_count[13] = 0xff;
  EXPECT_FALSE(platform::wire::decode_packet<jak2::wire::GungameStatePacket>(oversized_count));
  state.targets.front().spawn_time = -1;
  EXPECT_FALSE(platform::wire::encode_packet(jak2::wire::to_packet(state)));
  state.targets.front() = {.spawn_time = 1};
  EXPECT_FALSE(platform::wire::encode_packet(jak2::wire::to_packet(state)));
  state.targets.front() = {.state = jak2::core::GungameTargetState::SPAWNED};
  state.phase = jak2::core::GungamePhase::RED_INTRO;
  EXPECT_FALSE(platform::wire::encode_packet(jak2::wire::to_packet(state)));
  state.targets.clear();
  EXPECT_TRUE(platform::wire::encode_packet(jak2::wire::to_packet(state)));
  state.course_id = 2;
  EXPECT_FALSE(platform::wire::encode_packet(jak2::wire::to_packet(state)));
  state = {};
  EXPECT_TRUE(platform::wire::encode_packet(jak2::wire::to_packet(state)));
}
