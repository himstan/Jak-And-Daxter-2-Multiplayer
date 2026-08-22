#include <vector>

#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/sync/event_sync.h"
#include "gtest/gtest.h"

TEST(MultiplayerProtocol, BootstrapKeepsWireIdentityAndLayout) {
  EXPECT_EQ(static_cast<uint8_t>(PacketType::BOOTSTRAP), 4u);
  EXPECT_EQ(sizeof(PacketBootstrap), 761u);

  const auto* descriptor = multiplayer::schema::packet_descriptor(4);
  ASSERT_NE(descriptor, nullptr);
  EXPECT_STREQ(descriptor->name, "BOOTSTRAP");
  EXPECT_EQ(descriptor->allowed_roles, 1u);
  EXPECT_EQ(descriptor->priority, 1u);
  EXPECT_EQ(descriptor->max_payload, 2048u);
}

TEST(MultiplayerProtocol, JoinCarriesAValidatedNameReliably) {
  EXPECT_EQ(static_cast<uint8_t>(PacketType::EVENT_JOIN), 1u);
  EXPECT_EQ(sizeof(PacketJoin), 285u);

  const auto* descriptor = multiplayer::schema::packet_descriptor(1);
  ASSERT_NE(descriptor, nullptr);
  EXPECT_STREQ(descriptor->name, "EVENT_JOIN");
  EXPECT_EQ(descriptor->allowed_roles, 3u);
  EXPECT_EQ(descriptor->priority, 0u);
  EXPECT_EQ(descriptor->max_payload, 280u);
}

TEST(MultiplayerProtocol, LobbyAppearanceFitsTheReliableActionEnvelope) {
  EXPECT_EQ(sizeof(PacketLobbyAction), 273u);
  EXPECT_EQ(static_cast<uint8_t>(MPLobbyActionType::SET_APPEARANCE), 3u);
  const auto* descriptor = multiplayer::schema::packet_descriptor(14);
  ASSERT_NE(descriptor, nullptr);
  EXPECT_EQ(descriptor->allowed_roles, 3u);
  EXPECT_EQ(descriptor->priority, 0u);
  EXPECT_EQ(descriptor->max_payload, 268u);
}

TEST(MultiplayerProtocol, WelcomeCarriesTheHostAssignedCharacter) {
  EXPECT_EQ(static_cast<uint8_t>(PacketType::SESSION_WELCOME), 13u);
  EXPECT_EQ(sizeof(PacketSessionWelcome), 21u);

  const auto* descriptor = multiplayer::schema::packet_descriptor(13);
  ASSERT_NE(descriptor, nullptr);
  EXPECT_STREQ(descriptor->name, "SESSION_WELCOME");
  EXPECT_EQ(descriptor->allowed_roles, 1u);
  EXPECT_EQ(descriptor->max_payload, 16u);
}

TEST(MultiplayerEventCodec, EncodesOnlyTheDeclaredPayloadLength) {
  MPEvent event = {};
  event.etype = 34;
  event.payload_size = 3;
  event.source_player_id = 3;
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
  EXPECT_EQ(decoded.source_player_id, 3u);
  EXPECT_EQ(decoded.event_id, 34u);
  EXPECT_EQ(decoded.payload_size, 3u);
  EXPECT_EQ(decoded.payload[0], 0xaa);
  EXPECT_EQ(decoded.payload[2], 0xcc);
}

TEST(MultiplayerEventCodec, RejectsTruncationAndTrailingBytes) {
  MPEvent event = {};
  event.etype = 11;
  event.payload_size = 1;
  event.source_player_id = 2;
  event.data[0] = 4;
  std::vector<uint8_t> encoded;
  ASSERT_TRUE(mp_encode_game_event(event, 1, encoded));

  PacketGameEvent decoded = {};
  EXPECT_FALSE(mp_decode_game_event(encoded.data(), encoded.size() - 1, decoded));
  encoded.push_back(0);
  EXPECT_FALSE(mp_decode_game_event(encoded.data(), encoded.size(), decoded));
}

TEST(MultiplayerEventCodec, ZeroLengthUsesTheFixedPayload) {
  MPEvent event = {};
  event.etype = 11;
  event.source_player_id = 1;
  std::vector<uint8_t> encoded;
  ASSERT_TRUE(mp_encode_game_event(event, 2, encoded));
  EXPECT_EQ(encoded.size(), kEventEnvelopeHeaderWireSize + 480);
}

TEST(MultiplayerEventCodec, StampsLocalSourceAndRejectsSpoofedInboundSources) {
  MultiplayerData data;
  data.session_role = 0;
  data.local_player_id = 3;
  MPEventBufferGOAL events = {};
  events.out_count = 1;
  events.out_events[0].etype = 11;
  events.out_events[0].payload_size = 1;
  events.out_events[0].source_player_id = kMPInvalidPlayerId;
  mp_send_game_events(data, &events);
  EXPECT_EQ(events.out_events[0].source_player_id, 3u);
  EXPECT_EQ(events.out_count, 0u);

  MPEvent remote = {};
  remote.etype = 11;
  remote.payload_size = 1;
  remote.source_player_id = 2;
  std::vector<uint8_t> encoded;
  ASSERT_TRUE(mp_encode_game_event(remote, 7, encoded));
  ENetPacket packet = {};
  packet.data = encoded.data();
  packet.dataLength = encoded.size();
  mp_handle_game_event_packet(data, &packet, 2);
  EXPECT_EQ(data.inbound_events.size(), 1u);

  remote.source_player_id = 1;
  ASSERT_TRUE(mp_encode_game_event(remote, 8, encoded));
  packet.data = encoded.data();
  packet.dataLength = encoded.size();
  mp_handle_game_event_packet(data, &packet, 2);
  EXPECT_EQ(data.inbound_events.size(), 1u);

  MPEventBufferGOAL received = {};
  mp_receive_game_events(data, &received);
  ASSERT_EQ(received.in_count, 1u);
  EXPECT_EQ(received.in_events[0].source_player_id, 2u);

  remote.source_player_id = kMPInvalidPlayerId;
  EXPECT_FALSE(mp_encode_game_event(remote, 9, encoded));
}
