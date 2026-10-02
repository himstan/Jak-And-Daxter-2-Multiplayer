#include <span>
#include <vector>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/packets/pedestrian_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/vehicle_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/wire_policy.h"
#include "gtest/gtest.h"

TEST(Jak2Protocol, TrafficPrefixEncodesAndDecodesLosslessly) {
  multiplayer::jak2::wire::VehicleStateBatchPacket input = {};
  input.authority_revision = 99;
  input.level_id = 0xabcdef12;
  input.sample_time_ms = 0x12345678;

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(multiplayer::platform::wire::encode_packet(input, bytes));
  EXPECT_EQ(bytes.size(), multiplayer::jak2::wire::kVehicleStateBatchPacketPrefixWireSize);

  const auto decoded =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::VehicleStateBatchPacket>(
          bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->authority_revision, input.authority_revision);
  EXPECT_EQ(decoded->level_id, input.level_id);
  EXPECT_EQ(decoded->sample_time_ms, input.sample_time_ms);
}

TEST(Jak2Protocol, VehicleRecordEncodesThreeRidersAndReconstructsFourth) {
  multiplayer::jak2::wire::VehicleStateRecord input = {};
  input.net_id = 9001;
  input.vehicle_type = 4;
  input.color_index = 2;
  input.state_id = 7;
  input.position = {123.5f, -456.25f, 789.125f};
  input.quaternion = {0.1f, -0.2f, 0.3f, 0.4f};
  input.linear_velocity = {50, -50, 100};
  input.angular_velocity = {10, -10, 20};
  input.state_flags = 0x5;
  input.hit_points = 200;
  input.level_id = 12;
  input.rider_player_ids = {1, 2, multiplayer::jak2::wire::kWireCivilianRiderId};

  multiplayer::jak2::wire::VehicleStateBatchPacket packet = {};
  packet.vehicles.push_back(input);
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(multiplayer::platform::wire::encode_packet(packet, bytes));
  EXPECT_EQ(bytes.size(), 61u);

  const auto decoded_packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::VehicleStateBatchPacket>(
          bytes);
  ASSERT_TRUE(decoded_packet.has_value());
  ASSERT_EQ(decoded_packet->vehicles.size(), 1u);
  EXPECT_EQ(decoded_packet->vehicles[0].net_id, input.net_id);
  for (size_t axis = 0; axis < input.position.size(); ++axis) {
    EXPECT_NEAR(decoded_packet->vehicles[0].position[axis], input.position[axis],
                multiplayer::platform::wire::kVehiclePositionResolution / 2.0f);
    EXPECT_NEAR(decoded_packet->vehicles[0].linear_velocity[axis], input.linear_velocity[axis],
                multiplayer::platform::wire::kLinearVelocityResolution / 2.0f);
    EXPECT_NEAR(decoded_packet->vehicles[0].angular_velocity[axis], input.angular_velocity[axis],
                multiplayer::platform::wire::kAngularVelocityResolution / 2.0f);
  }
  for (size_t component = 0; component < input.quaternion.size(); ++component) {
    EXPECT_FLOAT_EQ(decoded_packet->vehicles[0].quaternion[component], input.quaternion[component]);
  }
  EXPECT_EQ(decoded_packet->vehicles[0].rider_player_ids, input.rider_player_ids);

  multiplayer::jak2::core::TrafficSnapshot decoded = {};
  multiplayer::jak2::wire::from_packet(*decoded_packet, decoded);
  ASSERT_EQ(decoded.vehicles.size(), 1u);
  EXPECT_EQ(decoded.vehicles[0].rider_player_ids[0], 1u);
  EXPECT_EQ(decoded.vehicles[0].rider_player_ids[1], 2u);
  EXPECT_EQ(decoded.vehicles[0].rider_player_ids[2],
            multiplayer::jak2::core::kTrafficVehicleCivilianRiderId);
  EXPECT_EQ(decoded.vehicles[0].rider_player_ids[3], multiplayer::jak2::core::kInvalidPlayerId);
  EXPECT_EQ(decoded.vehicles[0].target_player_id, 1u);
}

TEST(Jak2Protocol, VehicleRecordCivilianDriverReconstructsInvalidTarget) {
  multiplayer::jak2::wire::VehicleStateBatchPacket packet = {};
  packet.vehicles.push_back({.net_id = 1234,
                             .rider_player_ids = {multiplayer::jak2::wire::kWireCivilianRiderId,
                                                  multiplayer::jak2::wire::kWireInvalidPlayerId,
                                                  multiplayer::jak2::wire::kWireInvalidPlayerId}});

  const auto bytes = multiplayer::platform::wire::encode_packet(packet);
  ASSERT_TRUE(bytes.has_value());
  const auto decoded_packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::VehicleStateBatchPacket>(
          *bytes);
  ASSERT_TRUE(decoded_packet.has_value());
  multiplayer::jak2::core::TrafficSnapshot decoded = {};
  multiplayer::jak2::wire::from_packet(*decoded_packet, decoded);
  ASSERT_EQ(decoded.vehicles.size(), 1u);
  EXPECT_EQ(decoded.vehicles[0].rider_player_ids[0],
            multiplayer::jak2::core::kTrafficVehicleCivilianRiderId);
  EXPECT_EQ(decoded.vehicles[0].target_player_id, multiplayer::jak2::core::kInvalidPlayerId);
}

TEST(Jak2Protocol, PedestrianRecordEncodesAndDecodesLosslessly) {
  multiplayer::jak2::wire::PedestrianStateRecord input = {};
  input.net_id = 555;
  input.object_type = 2;
  input.appearance_mask = 0xff7ffeb1;
  input.position = {100.0f, 200.0f, 300.0f};
  input.quaternion = {0.1f, 0.2f, 0.3f, 0.4f};
  input.hit_points = 8;
  input.state_id = 23;
  input.flags = 0x3;
  input.target_player_id = 4;
  input.animation_profile = (14 << 8) | 5;
  input.vehicle_net_id = 777;
  input.transport_id = 888;
  input.transport_side = 1;
  input.level_id = 42;

  multiplayer::jak2::wire::PedestrianStateBatchPacket packet = {};
  packet.pedestrians.push_back(input);
  const auto bytes = multiplayer::platform::wire::encode_packet(packet);
  ASSERT_TRUE(bytes.has_value());
  const auto decoded_packet = multiplayer::platform::wire::decode_packet<
      multiplayer::jak2::wire::PedestrianStateBatchPacket>(*bytes);
  ASSERT_TRUE(decoded_packet.has_value());
  ASSERT_EQ(decoded_packet->pedestrians.size(), 1u);
  ASSERT_EQ(bytes->size(), 51u);
  EXPECT_EQ(std::vector<uint8_t>(bytes->begin() + 18, bytes->begin() + 22),
            (std::vector<uint8_t>{0xb1, 0xfe, 0x7f, 0xff}));
  EXPECT_EQ(decoded_packet->pedestrians[0].net_id, input.net_id);
  EXPECT_EQ(decoded_packet->pedestrians[0].appearance_mask, input.appearance_mask);
  for (size_t axis = 0; axis < input.position.size(); ++axis) {
    EXPECT_NEAR(decoded_packet->pedestrians[0].position[axis], input.position[axis],
                multiplayer::platform::wire::kPositionResolution / 2.0f);
  }
  for (size_t component = 0; component < input.quaternion.size(); ++component) {
    EXPECT_NEAR(decoded_packet->pedestrians[0].quaternion[component], input.quaternion[component],
                multiplayer::platform::wire::kQuaternionResolution / 2.0f);
  }
  EXPECT_EQ(decoded_packet->pedestrians[0].animation_profile, input.animation_profile);
  multiplayer::jak2::core::TrafficSnapshot snapshot;
  multiplayer::jak2::wire::from_packet(*decoded_packet, snapshot);
  ASSERT_EQ(snapshot.pedestrians.size(), 1u);
  EXPECT_EQ(snapshot.pedestrians[0].appearance_mask, input.appearance_mask);
  EXPECT_EQ(multiplayer::platform::wire::encode_packet(
                multiplayer::jak2::wire::to_pedestrian_state_batch_packet(snapshot)),
            bytes);
  for (size_t length = 0; length < bytes->size(); ++length) {
    EXPECT_FALSE(
        multiplayer::platform::wire::decode_packet<
            multiplayer::jak2::wire::PedestrianStateBatchPacket>(std::span(*bytes).first(length)));
  }
  auto trailing = *bytes;
  trailing.push_back(0);
  EXPECT_FALSE(multiplayer::platform::wire::decode_packet<
               multiplayer::jak2::wire::PedestrianStateBatchPacket>(trailing));
}

TEST(Jak2Protocol, PedestrianAppearancePreservesEveryScaleCodeWithoutGrowingPacket) {
  using namespace multiplayer;
  for (uint32_t width = 0; width < 16; ++width) {
    for (uint32_t height = 0; height < 16; ++height) {
      SCOPED_TRACE(width);
      SCOPED_TRACE(height);
      const uint32_t parts = ((width + height) & 1) ? 0x007ffeb1 : 0x00ffffff;
      const uint32_t appearance = parts | (width << 24) | (height << 28);
      jak2::core::TrafficSnapshot snapshot;
      snapshot.pedestrians.push_back({.net_id = 555, .appearance_mask = appearance});
      const auto bytes =
          platform::wire::encode_packet(jak2::wire::to_pedestrian_state_batch_packet(snapshot));
      ASSERT_TRUE(bytes);
      ASSERT_EQ(bytes->size(), 51u);
      EXPECT_EQ((*bytes)[21], width | (height << 4));
      const auto decoded =
          platform::wire::decode_packet<jak2::wire::PedestrianStateBatchPacket>(*bytes);
      ASSERT_TRUE(decoded);
      jak2::wire::from_packet(*decoded, snapshot);
      ASSERT_EQ(snapshot.pedestrians.size(), 1u);
      EXPECT_EQ(snapshot.pedestrians[0].appearance_mask, appearance);
      EXPECT_EQ(snapshot.pedestrians[0].appearance_mask & 0x00ffffff, parts);
    }
  }
}

TEST(Jak2Protocol, RejectsTruncatedBuffer) {
  multiplayer::jak2::wire::VehicleStateBatchPacket packet = {};
  packet.vehicles.push_back({.net_id = 1});
  const auto encoded = multiplayer::platform::wire::encode_packet(packet);
  ASSERT_TRUE(encoded.has_value());

  EXPECT_FALSE(
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::VehicleStateBatchPacket>(
          std::span(*encoded).first(encoded->size() - 1))
          .has_value());
  EXPECT_FALSE(
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::VehicleStateBatchPacket>(
          std::span(*encoded).first(
              multiplayer::jak2::wire::kVehicleStateBatchPacketPrefixWireSize - 1))
          .has_value());
}

TEST(Jak2Protocol, RejectsOversizedSnapshotsInsteadOfTruncating) {
  multiplayer::jak2::core::TrafficSnapshot pedestrians = {};
  pedestrians.pedestrians.resize(multiplayer::jak2::core::kMaxPedestrians);
  const auto full_batch = multiplayer::platform::wire::encode_packet(
      multiplayer::jak2::wire::to_pedestrian_state_batch_packet(pedestrians));
  ASSERT_TRUE(full_batch);
  EXPECT_LE(full_batch->size(),
            multiplayer::jak2::wire::PedestrianStateBatchPacket::kPolicy.maximum_payload_bytes);
  pedestrians.pedestrians.resize(multiplayer::jak2::core::kMaxPedestrians + 1);
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(
                   multiplayer::jak2::wire::to_pedestrian_state_batch_packet(pedestrians))
                   .has_value());

  multiplayer::jak2::core::TrafficSnapshot vehicles = {};
  vehicles.vehicles.resize(multiplayer::jak2::core::kMaxVehicles + 1);
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(
                   multiplayer::jak2::wire::to_vehicle_state_batch_packet(vehicles))
                   .has_value());
}
