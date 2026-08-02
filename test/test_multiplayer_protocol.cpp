#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/sync/event_sync.h"

#include "gtest/gtest.h"

#include <vector>

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
