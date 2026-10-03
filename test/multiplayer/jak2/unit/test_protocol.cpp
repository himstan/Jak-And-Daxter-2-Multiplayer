#include <span>
#include <vector>

#include "game/multiplayer/jak2/wire/event_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/jak2/wire/packets/bootstrap_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/game_event_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/player_vehicle_state_packet.h"
#include "gtest/gtest.h"
#include "test/multiplayer/jak2/unit/packet_test_helpers.h"

namespace mp_test = multiplayer::jak2::test;

namespace {

bool encode_event_batch(const multiplayer::jak2::core::GameEventBatch& batch,
                        std::vector<uint8_t>& bytes) {
  return multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(batch),
                                                    bytes);
}

bool decode_event_batch(const std::span<const uint8_t> bytes,
                        multiplayer::jak2::core::GameEventBatch& batch) {
  const auto packet =
      multiplayer::platform::wire::decode_packet<multiplayer::jak2::wire::GameEventBatchPacket>(
          bytes);
  if (!packet) {
    return false;
  }
  multiplayer::jak2::wire::from_packet(*packet, batch);
  return true;
}

}  // namespace

TEST(Jak2Protocol, EncodesOnlyTheDeclaredPayloadLength) {
  multiplayer::jak2::core::GameEvent event = {};
  event.event_id = 26;
  event.payload_size = 13;
  event.source_player_id = 3;
  event.payload[0] = 0xaa;
  event.payload[1] = 0xbb;
  event.payload[2] = 0xcc;

  std::vector<uint8_t> encoded;
  const multiplayer::jak2::core::GameEventBatch batch = {.events = {event}};
  ASSERT_TRUE(encode_event_batch(batch, encoded));
  EXPECT_EQ(encoded.size(), multiplayer::jak2::wire::kGameEventBatchPacketPrefixWireSize +
                                multiplayer::jak2::wire::kGameEventRecordPrefixWireSize + 13);

  multiplayer::jak2::core::GameEventBatch decoded = {};
  ASSERT_TRUE(decode_event_batch(encoded, decoded));
  ASSERT_EQ(decoded.events.size(), 1u);
  EXPECT_EQ(decoded.events[0].event_id, 26u);
  EXPECT_EQ(decoded.events[0].payload_size, 13u);
  EXPECT_EQ(decoded.events[0].payload[0], 0xaa);
  EXPECT_EQ(decoded.events[0].payload[2], 0xcc);
}

TEST(Jak2Protocol, EventCodecRejectsTruncationAndTrailingBytes) {
  multiplayer::jak2::core::GameEvent event = {};
  event.event_id = 30;
  event.payload_size = 1;
  event.source_player_id = 2;
  event.payload[0] = 4;
  std::vector<uint8_t> encoded;
  const multiplayer::jak2::core::GameEventBatch batch = {.events = {event}};
  ASSERT_TRUE(encode_event_batch(batch, encoded));

  multiplayer::jak2::core::GameEventBatch decoded = {};
  EXPECT_FALSE(decode_event_batch(std::span(encoded).first(encoded.size() - 1), decoded));
  encoded.push_back(0);
  EXPECT_FALSE(decode_event_batch(encoded, decoded));
}

TEST(Jak2Protocol, SupportsDefinedZeroLengthPayloads) {
  multiplayer::jak2::core::GameEvent event = {};
  event.event_id = 12;
  event.source_player_id = 1;
  std::vector<uint8_t> encoded;
  const multiplayer::jak2::core::GameEventBatch batch = {.events = {event}};
  ASSERT_TRUE(encode_event_batch(batch, encoded));
  EXPECT_EQ(encoded.size(), 3u);

  multiplayer::jak2::core::GameEventBatch decoded = {};
  ASSERT_TRUE(decode_event_batch(encoded, decoded));
  ASSERT_EQ(decoded.events.size(), 1u);
  EXPECT_EQ(decoded.events[0].payload_size, 0u);
}

TEST(Jak2Protocol, EveryEventUsesItsDeclaredPayloadSize) {
  for (const auto& [id, name, payload_size] : multiplayer::jak2::wire::kEvents) {
    multiplayer::jak2::core::GameEvent event = {};
    event.event_id = id;
    event.source_player_id = 0;
    event.payload_size = payload_size;
    std::vector<uint8_t> encoded;
    const multiplayer::jak2::core::GameEventBatch batch = {.events = {event}};
    ASSERT_TRUE(encode_event_batch(batch, encoded)) << name;
    EXPECT_EQ(encoded.size(), multiplayer::jak2::wire::kGameEventBatchPacketPrefixWireSize +
                                  multiplayer::jak2::wire::kGameEventRecordPrefixWireSize +
                                  payload_size)
        << name;
    multiplayer::jak2::core::GameEventBatch decoded = {};
    ASSERT_TRUE(decode_event_batch(encoded, decoded)) << name;
    ASSERT_EQ(decoded.events.size(), 1u) << name;
    EXPECT_EQ(decoded.events[0].event_id, id) << name;
    EXPECT_EQ(decoded.events[0].payload_size, payload_size) << name;
  }
}

TEST(Jak2Protocol, GenericCodecCanonicalizesAuthenticatedEventOrigin) {
  multiplayer::jak2::core::GameEvent event = {};
  event.event_id = 30;
  event.payload_size = 1;
  event.source_player_id = 2;
  event.payload[0] = 4;
  const auto outbound = multiplayer::jak2::core::GameEventBatch{.events = {event}};
  const auto encoded =
      multiplayer::platform::wire::encode_packet(multiplayer::jak2::wire::to_packet(outbound));
  ASSERT_TRUE(encoded.has_value());

  const multiplayer::jak2::core::SourceContext source = {
      .connection_id = 1, .authenticated_player_id = 2, .from_host = false};
  const auto decoded = mp_test::decode_events(*encoded, source);
  ASSERT_TRUE(decoded);
  const auto* decoded_batch = &*decoded;
  ASSERT_NE(decoded_batch, nullptr);
  ASSERT_EQ(decoded_batch->events.size(), 1u);
  EXPECT_EQ(decoded_batch->events[0].source_player_id, 2u);
}

TEST(Jak2Protocol, GameplayPolicyMaximumAppliesToPayloadBody) {
  const auto* policy = (&multiplayer::jak2::wire::GameEventBatchPacket::kPolicy);
  ASSERT_NE(policy, nullptr);
  EXPECT_EQ(policy->maximum_payload_bytes, 8192u);
}

TEST(Jak2Protocol, PlayerVehicleStateCarriesDriverVehicleWireLayout) {
  EXPECT_EQ(static_cast<uint8_t>(PacketType::PLAYER_VEHICLE_STATE), 11u);
  EXPECT_EQ(multiplayer::jak2::wire::kPlayerVehicleStatePacketWireSize, 53u);

  const auto* policy = (&multiplayer::jak2::wire::PlayerVehicleStatePacket::kPolicy);
  ASSERT_NE(policy, nullptr);
  EXPECT_STREQ(policy->name, "PLAYER_VEHICLE_STATE");
  EXPECT_EQ(policy->direction, multiplayer::platform::MessageDirection::BIDIRECTIONAL);
  EXPECT_EQ(policy->maximum_payload_bytes, 53u);
}
