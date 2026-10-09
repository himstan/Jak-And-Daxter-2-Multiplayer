#include <span>
#include <vector>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/event_types.h"
#include "game/multiplayer/jak2/wire/packets/player_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/player_vehicle_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/turret_state_packet.h"
#include "game/multiplayer/platform/core/message_policy.h"
#include "gtest/gtest.h"

TEST(Jak2Protocol, RoundTripsCorePlayerStateWithStableWireSize) {
  multiplayer::jak2::core::PlayerState input = {};
  input.player_id = 3;
  input.state_ready = true;
  input.scene_active = true;
  input.spectator_only = true;
  input.position = {1.0f, 2.0f, 3.0f};
  input.velocity = {-4.0f, 5.0f, -6.0f};
  input.angle = 1.5f;
  input.camera_angle_y = -0.75f;
  input.levels[0] = {.level_id = 0x44, .flags = 0x01};
  input.levels[1] = {.level_id = 0x33, .flags = 0x06};
  input.levels[2] = {.level_id = 0x22, .flags = 0x08};
  input.levels[3] = {.level_id = 0x11, .flags = 0x03};
  input.levels[4] = {.level_id = 0x55, .flags = 0x12};
  input.levels[5] = {.level_id = 0x66, .flags = 0xa3};
  input.state_id = 17;
  input.action_sequence = 9;
  input.action_state_id = 11;
  input.vehicle_id = 0x11223344;
  input.vehicle_seat = 2;
  input.riding_along_player_id = 4;
  input.selected_traffic_authority = 7;
  input.mission_flags = 0x2;
  input.visual_secrets = 0x4;

  const auto packet = multiplayer::jak2::wire::to_packet(input);
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(multiplayer::platform::wire::encode_packet(packet, bytes));
  ASSERT_EQ(bytes.size(),
            (&multiplayer::jak2::wire::PlayerStatePacket::kPolicy)->maximum_payload_bytes);

  const auto decoded_packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::PlayerStatePacket>(bytes);
  ASSERT_TRUE(decoded_packet.has_value());
  multiplayer::jak2::core::PlayerState output = {};
  multiplayer::jak2::wire::from_packet(*decoded_packet, output);
  EXPECT_EQ(output.player_id, multiplayer::jak2::core::kInvalidPlayerId);
  for (size_t axis = 0; axis < input.position.size(); ++axis) {
    EXPECT_NEAR(output.position[axis], input.position[axis],
                multiplayer::platform::wire::kPositionResolution / 2.0f);
    EXPECT_NEAR(output.velocity[axis], input.velocity[axis],
                multiplayer::platform::wire::kLinearVelocityResolution / 2.0f);
  }
  EXPECT_EQ(output.state_id, input.state_id);
  for (size_t level = 0; level < input.levels.size(); ++level) {
    EXPECT_EQ(output.levels[level].level_id, input.levels[level].level_id);
    EXPECT_EQ(output.levels[level].flags, input.levels[level].flags);
  }
  EXPECT_EQ(output.vehicle_id, input.vehicle_id);
  EXPECT_EQ(output.selected_traffic_authority, input.selected_traffic_authority);
  EXPECT_TRUE(output.spectator_only);
  EXPECT_TRUE(output.scene_active);
}

TEST(Jak2Protocol, TrafficInterestFitsPlayerPacketAndRejectsInvalidSources) {
  using namespace multiplayer;
  for (const auto source : {0, 1, 2, 3, 4, 5, 6, 7, 255}) {
    jak2::core::PlayerState input;
    input.selected_traffic_authority = static_cast<uint8_t>(source);
    const auto bytes = platform::wire::encode_packet(jak2::wire::to_packet(input));
    ASSERT_TRUE(bytes);
    EXPECT_EQ(bytes->size(), 50u);
    const auto packet = platform::wire::decode_packet<jak2::wire::PlayerStatePacket>(*bytes);
    ASSERT_TRUE(packet);
    jak2::core::PlayerState output;
    jak2::wire::from_packet(*packet, output);
    EXPECT_EQ(output.selected_traffic_authority, source);
  }
  for (uint8_t source = 8; source < 15; ++source) {
    jak2::wire::PlayerStatePacket invalid;
    invalid.selected_traffic_authority = source;
    EXPECT_FALSE(platform::wire::encode_packet(invalid));
    auto bytes = platform::wire::encode_packet(jak2::wire::PlayerStatePacket{});
    ASSERT_TRUE(bytes);
    (*bytes)[48] = ((*bytes)[48] & 0x3f) | ((source & 3) << 6);
    (*bytes)[49] = source >> 2;
    EXPECT_FALSE(platform::wire::decode_packet<jak2::wire::PlayerStatePacket>(*bytes));
  }
}

TEST(Jak2Protocol, HitInvulnerabilityRoundTrips) {
  using namespace multiplayer;
  for (const bool hit_invulnerable : {false, true}) {
    for (uint8_t respawn_flags = 0; respawn_flags < 8; ++respawn_flags) {
      jak2::core::PlayerState input;
      input.hit_invulnerable = hit_invulnerable;
      input.respawn_flags = respawn_flags;
      const auto bytes = platform::wire::encode_packet(jak2::wire::to_packet(input));
      ASSERT_TRUE(bytes);
      EXPECT_EQ(bytes->size(), 50u);
      const auto packet = platform::wire::decode_packet<jak2::wire::PlayerStatePacket>(*bytes);
      ASSERT_TRUE(packet);
      jak2::core::PlayerState output;
      jak2::wire::from_packet(*packet, output);
      EXPECT_EQ(output.hit_invulnerable, hit_invulnerable);
      EXPECT_EQ(output.respawn_flags, respawn_flags);
    }
  }
}

TEST(Jak2Protocol, PlayerCodecRejectsTruncationAndTrailingBytes) {
  multiplayer::jak2::core::PlayerState input = {};
  input.player_id = 1;
  const auto packet = multiplayer::jak2::wire::to_packet(input);
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(multiplayer::platform::wire::encode_packet(packet, bytes));

  EXPECT_FALSE(
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::PlayerStatePacket>(
          std::span(bytes).first(bytes.size() - 1))
          .has_value());
  bytes.push_back(0);
  EXPECT_FALSE(
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::PlayerStatePacket>(bytes)
          .has_value());
}

TEST(Jak2Protocol, PlayerCodecHandlesOverflowingActionSequenceSafely) {
  multiplayer::jak2::core::PlayerState input = {};
  input.player_id = 1;
  input.state_ready = true;
  input.action_sequence = 256;
  input.action_state_id = 260;
  input.state_id = 300;

  const auto packet = multiplayer::jak2::wire::to_packet(input);
  EXPECT_EQ(packet.action_sequence, 0u);
  EXPECT_EQ(packet.action_state_id, 4u);
  EXPECT_EQ(packet.state_id, 44u);
  EXPECT_TRUE(multiplayer::jak2::wire::validate_packet(packet));

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(multiplayer::platform::wire::encode_packet(packet, bytes));
  EXPECT_EQ(bytes.size(),
            (&multiplayer::jak2::wire::PlayerStatePacket::kPolicy)->maximum_payload_bytes);

  const auto decoded_packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::PlayerStatePacket>(bytes);
  ASSERT_TRUE(decoded_packet.has_value());
  EXPECT_EQ(decoded_packet->action_sequence, 0u);
  EXPECT_EQ(decoded_packet->action_state_id, 4u);
  EXPECT_EQ(decoded_packet->state_id, 44u);

  multiplayer::jak2::core::PlayerState output = {};
  multiplayer::jak2::wire::from_packet(*decoded_packet, output);
  EXPECT_EQ(output.action_sequence, 0u);
  EXPECT_EQ(output.action_state_id, 4u);
  EXPECT_EQ(output.state_id, 44u);
}

TEST(Jak2Protocol, RoundTripsPlayerVehicleAndTurretState) {
  multiplayer::jak2::core::PlayerVehicleState vehicle = {};
  vehicle.player_id = 2;
  vehicle.vehicle.net_id = 0x12345678;
  vehicle.vehicle.vehicle_type = 4;
  vehicle.vehicle.position = {1.0f, -2.0f, 3.0f};
  vehicle.vehicle.quaternion = {0.0f, 0.0f, 0.5f, 0.5f};
  vehicle.vehicle.linear_velocity = {4.0f, 5.0f, 6.0f};
  vehicle.vehicle.angular_velocity = {-1.0f, -2.0f, -3.0f};
  vehicle.vehicle.rider_player_ids = {2, 3, multiplayer::jak2::core::kInvalidPlayerId,
                                      multiplayer::jak2::core::kTrafficVehicleCivilianRiderId};

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(multiplayer::platform::wire::encode_packet(
      multiplayer::jak2::wire::to_packet(vehicle), bytes));
  ASSERT_EQ(bytes.size(), multiplayer::jak2::wire::kPlayerVehicleStatePacketWireSize);
  const auto decoded_vehicle_packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::PlayerVehicleStatePacket>(
          bytes);
  ASSERT_TRUE(decoded_vehicle_packet.has_value());
  multiplayer::jak2::core::PlayerVehicleState decoded_vehicle = {};
  multiplayer::jak2::wire::from_packet(*decoded_vehicle_packet, decoded_vehicle);
  EXPECT_EQ(decoded_vehicle.vehicle.net_id, vehicle.vehicle.net_id);
  for (size_t axis = 0; axis < vehicle.vehicle.position.size(); ++axis) {
    EXPECT_NEAR(decoded_vehicle.vehicle.position[axis], vehicle.vehicle.position[axis],
                multiplayer::platform::wire::kVehiclePositionResolution / 2.0f);
  }
  EXPECT_EQ(decoded_vehicle.vehicle.quaternion, vehicle.vehicle.quaternion);
  EXPECT_EQ(decoded_vehicle.vehicle.rider_player_ids, vehicle.vehicle.rider_player_ids);

  multiplayer::jak2::core::TurretState turret = {};
  turret.player_id = 2;
  turret.turret_aid = 0xabcdef;
  turret.rotation_y = 1.25f;
  turret.rotation_x = -0.5f;
  ASSERT_TRUE(multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(turret),
                                                         bytes));
  ASSERT_EQ(bytes.size(), multiplayer::jak2::wire::kTurretStatePacketWireSize);
  const auto decoded_turret_packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::TurretStatePacket>(bytes);
  ASSERT_TRUE(decoded_turret_packet.has_value());
  multiplayer::jak2::core::TurretState decoded_turret = {};
  multiplayer::jak2::wire::from_packet(*decoded_turret_packet, decoded_turret);
  EXPECT_EQ(decoded_turret.turret_aid, turret.turret_aid);
  EXPECT_NEAR(decoded_turret.rotation_y, turret.rotation_y,
              multiplayer::platform::wire::kAngleResolution / 2.0f);
  EXPECT_NEAR(decoded_turret.rotation_x, turret.rotation_x,
              multiplayer::platform::wire::kAngleResolution / 2.0f);
}
