#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_security.h"
#include "game/multiplayer/multiplayer_wire_codec.h"

#include "gtest/gtest.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
void authenticate(MultiplayerSecurity& host, MultiplayerSecurity& client) {
  MultiplayerDatagram server_hello;
  ASSERT_TRUE(host.make_server_hello(server_hello));
  SecurityReceiveResult client_proof =
      client.receive(1, server_hello.bytes.data(), server_hello.size);
  ASSERT_EQ(client_proof.kind, SecurityReceiveKind::HANDSHAKE);
  ASSERT_GT(client_proof.response.size, 0);

  SecurityReceiveResult server_proof = host.receive(
      0, client_proof.response.bytes.data(), client_proof.response.size);
  ASSERT_EQ(server_proof.kind, SecurityReceiveKind::HANDSHAKE);
  ASSERT_TRUE(host.authenticated());

  SecurityReceiveResult client_result =
      client.receive(1, server_proof.response.bytes.data(), server_proof.response.size);
  ASSERT_EQ(client_result.kind, SecurityReceiveKind::HANDSHAKE);
  ASSERT_TRUE(client.authenticated());
}
}  // namespace

TEST(MultiplayerPacket, SaturatingQuantizationRejectsNonFiniteValues) {
  EXPECT_EQ(mp_pack_float_q(std::numeric_limits<float>::quiet_NaN()), 0);
  EXPECT_EQ(mp_pack_float_scaled(std::numeric_limits<float>::infinity(), 10.0f), 0);
  EXPECT_EQ(mp_pack_float_scaled(100000.0f, 10.0f),
            (std::numeric_limits<int16_t>::max)());
  EXPECT_EQ(mp_pack_float_scaled(-100000.0f, 10.0f),
            (std::numeric_limits<int16_t>::min)());
}

TEST(MultiplayerPacket, SequenceComparisonHandlesWraparound) {
  EXPECT_TRUE(mp_sequence_is_newer(1, 0));
  EXPECT_TRUE(mp_sequence_is_newer(0, (std::numeric_limits<uint32_t>::max)()));
  EXPECT_FALSE(mp_sequence_is_newer(9, 10));
  EXPECT_FALSE(mp_sequence_is_newer(10, 10));
}

TEST(MultiplayerPacket, PacketViewRejectsTruncationUnknownTypesAndTrailingData) {
  PacketPlayerState state = {};
  state.header.type = PacketType::STATE_UPDATE;
  std::vector<uint8_t> bytes(sizeof(state) + 1);
  memcpy(bytes.data(), &state, sizeof(state));
  ENetPacket packet = {};
  packet.data = bytes.data();

  packet.dataLength = sizeof(PacketHeader) - 1;
  EXPECT_FALSE(PacketView(&packet).has_header());

  packet.dataLength = sizeof(state) + 1;
  EXPECT_FALSE(PacketView(&packet).as_exact<PacketPlayerState>(PacketType::STATE_UPDATE));

  packet.dataLength = sizeof(state);
  bytes[0] = static_cast<uint8_t>(PacketType::COUNT);
  EXPECT_EQ(PacketView(&packet).type(), PacketType::COUNT);
  EXPECT_FALSE(PacketView(&packet).as_exact<PacketPlayerState>(PacketType::STATE_UPDATE));
}

TEST(MultiplayerPacket, CountedPayloadRejectsOverflowAndTrailingData) {
  std::array<uint8_t, 32> bytes = {};
  ENetPacket packet = {};
  packet.data = bytes.data();
  packet.dataLength = bytes.size();
  PacketView view(&packet);
  EXPECT_FALSE(view.has_counted_payload((std::numeric_limits<uint32_t>::max)(),
                                        (std::numeric_limits<size_t>::max)(), 17));
  EXPECT_FALSE(view.has_counted_payload(1, 4, 17));
  packet.dataLength = 21;
  EXPECT_TRUE(view.has_counted_payload(1, 4, 17));
}

TEST(MultiplayerPacket, DirectionPolicyMatchesHostAndClientRoles) {
  for (uint8_t value = 0; value < static_cast<uint8_t>(PacketType::COUNT); ++value) {
    const PacketType type = static_cast<PacketType>(value);
    const bool host_only = type == PacketType::FULL_SYNC ||
                           type == PacketType::PEDESTRIAN_SYNC ||
                           type == PacketType::VEHICLE_SYNC ||
                           type == PacketType::PALACE_SQUID_SYNC;
    const bool gameplay = type != PacketType::EVENT_JOIN && type != PacketType::EVENT_LEAVE;
    EXPECT_EQ(mp_packet_direction_allowed(type, 0), gameplay);
    EXPECT_EQ(mp_packet_direction_allowed(type, 1), gameplay && !host_only);
  }
  EXPECT_FALSE(mp_packet_direction_allowed(PacketType::STATE_UPDATE, -1));
  EXPECT_FALSE(mp_packet_direction_allowed(PacketType::COUNT, 0));
}

TEST(MultiplayerWireCodec, UsesLittleEndianAndRequiresExactConsumption) {
  std::array<uint8_t, 10> bytes = {};
  multiplayer::wire::store_u16_le(bytes.data(), 0x1234);
  multiplayer::wire::store_u64_le(bytes.data() + 2, UINT64_C(0x0102030405060708));
  EXPECT_EQ(bytes[0], 0x34);
  EXPECT_EQ(bytes[9], 0x01);

  multiplayer::wire::Reader reader(bytes.data(), bytes.size());
  uint16_t decoded_u16 = 0;
  uint64_t decoded_u64 = 0;
  EXPECT_TRUE(reader.read_u16(decoded_u16));
  EXPECT_TRUE(reader.read_u64(decoded_u64));
  EXPECT_TRUE(reader.consumed_all());
  EXPECT_EQ(decoded_u16, 0x1234);
  EXPECT_EQ(decoded_u64, UINT64_C(0x0102030405060708));

  multiplayer::wire::Reader truncated(bytes.data(), bytes.size() - 1);
  EXPECT_TRUE(truncated.read_u16(decoded_u16));
  EXPECT_FALSE(truncated.read_u64(decoded_u64));
  EXPECT_FALSE(truncated.consumed_all());
}

TEST(MultiplayerSecurity, InviteRoundTripAndMutualAuthentication) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  const std::string invite = host.invite_for_address("127.0.0.1");
  EXPECT_EQ(invite.size(), 38);
  EXPECT_EQ(invite.substr(invite.rfind('/') + 1).size(), 22);
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(invite, parsed_host, parsed_port));
  EXPECT_EQ(parsed_host, "127.0.0.1");
  EXPECT_EQ(parsed_port, 26210);
  authenticate(host, client);
}

TEST(MultiplayerSecurity, RejectsMalformedAndNonIpv4Invites) {
  MultiplayerSecurity client;
  std::string host;
  uint16_t port = 0;
  EXPECT_FALSE(client.start_client("127.0.0.1:26210", host, port));
  EXPECT_FALSE(client.start_client(
      "ogmp://127.0.0.1:26210/AAAAAAAAAAAAAAAAAAAAAA", host, port));
  EXPECT_FALSE(client.start_client(
      "example.com:26210/AAAAAAAAAAAAAAAAAAAAAA", host, port));
  EXPECT_FALSE(client.start_client(
      "999.0.0.1:26210/AAAAAAAAAAAAAAAAAAAAAA", host, port));
  EXPECT_FALSE(client.start_client(
      "127.0.0.1:0/AAAAAAAAAAAAAAAAAAAAAA", host, port));
  EXPECT_FALSE(client.start_client(
      "127.0.0.1:26210/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", host, port));
  EXPECT_FALSE(client.start_client(
      "127.0.0.1:26210/AAAAAAAAAAAAAAAAAAAAAA/trailing", host, port));
}

TEST(MultiplayerSecurity, EncryptsAuthenticatesAndRejectsReplay) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(host.invite_for_address("127.0.0.1"), parsed_host,
                                  parsed_port));
  authenticate(host, client);

  PacketHeader header = {};
  header.type = PacketType::EVENT_GAME;
  header.sequenceNum = 42;
  MultiplayerDatagram encrypted;
  ASSERT_TRUE(host.seal(0, header.type, &header, sizeof(header), encrypted));

  SecurityReceiveResult first = client.receive(1, encrypted.bytes.data(), encrypted.size);
  ASSERT_EQ(first.kind, SecurityReceiveKind::GAMEPLAY);
  ASSERT_EQ(first.plaintext.size, sizeof(header));

  SecurityReceiveResult replay = client.receive(1, encrypted.bytes.data(), encrypted.size);
  EXPECT_EQ(replay.kind, SecurityReceiveKind::REJECTED);
}

TEST(MultiplayerSecurity, RejectsTamperingAndWrongInviteToken) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string invite = host.invite_for_address("127.0.0.1");
  invite.back() = invite.back() == 'A' ? 'B' : 'A';
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(invite, parsed_host, parsed_port));

  MultiplayerDatagram server_hello;
  ASSERT_TRUE(host.make_server_hello(server_hello));
  SecurityReceiveResult client_proof =
      client.receive(1, server_hello.bytes.data(), server_hello.size);
  ASSERT_GT(client_proof.response.size, 0);
  SecurityReceiveResult rejected = host.receive(
      0, client_proof.response.bytes.data(), client_proof.response.size);
  EXPECT_EQ(rejected.kind, SecurityReceiveKind::REJECTED);
  EXPECT_FALSE(host.authenticated());
}

TEST(MultiplayerSecurity, RejectsAlteredCiphertextAndAcceptsReordering) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(host.invite_for_address("127.0.0.1"), parsed_host,
                                  parsed_port));
  authenticate(host, client);

  PacketHeader header = {PacketType::EVENT_GAME, 1};
  MultiplayerDatagram first;
  MultiplayerDatagram second;
  MultiplayerDatagram third;
  ASSERT_TRUE(host.seal(0, header.type, &header, sizeof(header), first));
  header.sequenceNum = 2;
  ASSERT_TRUE(host.seal(0, header.type, &header, sizeof(header), second));
  header.sequenceNum = 3;
  ASSERT_TRUE(host.seal(0, header.type, &header, sizeof(header), third));

  first.bytes[first.size - 1] ^= 0x80;
  EXPECT_EQ(client.receive(1, first.bytes.data(), first.size).kind,
            SecurityReceiveKind::REJECTED);
  EXPECT_EQ(client.receive(1, third.bytes.data(), third.size).kind,
            SecurityReceiveKind::GAMEPLAY);
  EXPECT_EQ(client.receive(1, second.bytes.data(), second.size).kind,
            SecurityReceiveKind::GAMEPLAY);
}
