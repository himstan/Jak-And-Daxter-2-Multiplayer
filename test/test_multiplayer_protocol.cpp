#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/sync/event_sync.h"

#include "gtest/gtest.h"

#include <vector>

TEST(MultiplayerProtocol, BootstrapKeepsWireIdentityAndLayout) {
  EXPECT_EQ(static_cast<uint8_t>(PacketType::BOOTSTRAP), 4u);
  EXPECT_EQ(kMultiplayerWireRevision, 4u);
  EXPECT_EQ(sizeof(PacketBootstrap), 769u);

  const auto* descriptor = multiplayer::schema::packet_descriptor(4);
  ASSERT_NE(descriptor, nullptr);
  EXPECT_STREQ(descriptor->name, "BOOTSTRAP");
  EXPECT_EQ(descriptor->allowed_roles, 1u);
  EXPECT_EQ(descriptor->priority, 1u);
  EXPECT_EQ(descriptor->max_payload, 2048u);
}

TEST(MultiplayerEventCodec, EncodesOnlyTheDeclaredPayloadLength) {
  MPEvent event = {};
  event.etype = 34;
  event.payload_size = 3;
  event.data[0] = 0xaa;
  event.data[1] = 0xbb;
  event.data[2] = 0xcc;

  std::vector<uint8_t> encoded;
  ASSERT_TRUE(mp_encode_game_event(event, 7, encoded));
  EXPECT_EQ(encoded.size(), kEventEnvelopeHeaderWireSize + 3);

  PacketGameEvent decoded = {};
  ASSERT_TRUE(mp_decode_game_event(encoded.data(), encoded.size(), decoded));
  EXPECT_EQ(decoded.header.type, PacketType::EVENT_GAME);
  EXPECT_EQ(decoded.header.sequenceNum, 7u);
  EXPECT_EQ(decoded.event_id, 34u);
  EXPECT_EQ(decoded.payload_size, 3u);
  EXPECT_EQ(decoded.payload[0], 0xaa);
  EXPECT_EQ(decoded.payload[2], 0xcc);
}

TEST(MultiplayerEventCodec, RejectsTruncationAndTrailingBytes) {
  MPEvent event = {};
  event.etype = 11;
  event.payload_size = 1;
  event.data[0] = 4;
  std::vector<uint8_t> encoded;
  ASSERT_TRUE(mp_encode_game_event(event, 1, encoded));

  PacketGameEvent decoded = {};
  EXPECT_FALSE(mp_decode_game_event(encoded.data(), encoded.size() - 1, decoded));
  encoded.push_back(0);
  EXPECT_FALSE(mp_decode_game_event(encoded.data(), encoded.size(), decoded));
}

TEST(MultiplayerEventCodec, LegacyZeroLengthUsesTheCompatibilityPayload) {
  MPEvent event = {};
  event.etype = 11;
  std::vector<uint8_t> encoded;
  ASSERT_TRUE(mp_encode_game_event(event, 2, encoded));
  EXPECT_EQ(encoded.size(), kEventEnvelopeHeaderWireSize + 480);
}
