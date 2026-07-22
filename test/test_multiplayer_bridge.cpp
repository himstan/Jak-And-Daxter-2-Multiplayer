#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_api.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_port_mapping.h"
#include "game/multiplayer/multiplayer_scanner.h"
#include "game/multiplayer/multiplayer_security.h"
#include "game/multiplayer/multiplayer_session.h"
#include "game/multiplayer/multiplayer_version.h"
#include "game/multiplayer/multiplayer_wire_codec.h"
#include "gtest/gtest.h"

namespace {
void authenticate(MultiplayerSecurity& host, MultiplayerSecurity& client) {
  ASSERT_TRUE(host.set_local_version("v1.0.0"));
  ASSERT_TRUE(client.set_local_version("v1.0.0"));
  MultiplayerDatagram server_hello;
  ASSERT_TRUE(host.make_server_hello(server_hello));
  SecurityReceiveResult client_proof =
      client.receive(1, server_hello.bytes.data(), server_hello.size);
  ASSERT_EQ(client_proof.kind, SecurityReceiveKind::HANDSHAKE);
  ASSERT_GT(client_proof.response.size, 0);

  SecurityReceiveResult server_proof =
      host.receive(0, client_proof.response.bytes.data(), client_proof.response.size);
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
  EXPECT_EQ(mp_pack_float_scaled(100000.0f, 10.0f), (std::numeric_limits<int16_t>::max)());
  EXPECT_EQ(mp_pack_float_scaled(-100000.0f, 10.0f), (std::numeric_limits<int16_t>::min)());
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
    const bool host_only = type == PacketType::FULL_SYNC || type == PacketType::PEDESTRIAN_SYNC ||
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

TEST(MultiplayerDiscovery, RequiresExactBoundedTokenResponse) {
  const std::string valid = std::string(DISCOVERY_MAGIC) + "|AAAAAAAAAAAAAAAAAAAAAA";
  std::string token;
  ASSERT_TRUE(mp_parse_discovery_response(valid.data(), valid.size(), token));
  EXPECT_EQ(token, "AAAAAAAAAAAAAAAAAAAAAA");
  EXPECT_FALSE(mp_parse_discovery_response(valid.data(), valid.size() - 1, token));
  EXPECT_FALSE(mp_parse_discovery_response((valid + "x").data(), valid.size() + 1, token));
  std::string invalid = valid;
  invalid.back() = '/';
  EXPECT_FALSE(mp_parse_discovery_response(invalid.data(), invalid.size(), token));
}

TEST(MultiplayerDiscovery, CancellationClearsPrivateResultState) {
  MultiplayerData data;
  data.found_ip = "25.1.2.3:26210/AAAAAAAAAAAAAAAAAAAAAA";
  data.directed_discovery = true;
  data.directed_discovery_address = 1;
  data.directed_discovery_game_port = 26210;
  MultiplayerScanner::stop_search(data);
  EXPECT_TRUE(data.found_ip.empty());
  EXPECT_FALSE(data.directed_discovery);
  EXPECT_EQ(data.directed_discovery_address, 0u);
  EXPECT_EQ(data.directed_discovery_game_port, 0);
}

TEST(MultiplayerDiscovery, DirectedSearchAcceptsOnlyBareIpv4AndValidPort) {
  MultiplayerData data;
  EXPECT_FALSE(MultiplayerScanner::start_direct_search(data, "localhost", 26210));
  EXPECT_FALSE(MultiplayerScanner::start_direct_search(data, "127.0.0.1:26210", 26210));
  EXPECT_FALSE(MultiplayerScanner::start_direct_search(data, "127.0.0.1", 0));
  ASSERT_TRUE(MultiplayerScanner::start_direct_search(data, "127.0.0.1", 26210));
  MultiplayerScanner::stop_search(data);
}

TEST(MultiplayerPortMapping, ClassifiesPublicInviteAddresses) {
  EXPECT_TRUE(mp_is_public_ipv4("8.8.8.8"));
  EXPECT_TRUE(mp_is_public_ipv4("25.1.2.3"));
  EXPECT_TRUE(mp_is_public_ipv4("203.0.114.1"));
  EXPECT_FALSE(mp_is_public_ipv4("127.0.0.1"));
  EXPECT_FALSE(mp_is_public_ipv4("10.0.0.1"));
  EXPECT_FALSE(mp_is_public_ipv4("100.64.0.1"));
  EXPECT_FALSE(mp_is_public_ipv4("169.254.1.1"));
  EXPECT_FALSE(mp_is_public_ipv4("172.16.0.1"));
  EXPECT_FALSE(mp_is_public_ipv4("192.168.0.1"));
  EXPECT_FALSE(mp_is_public_ipv4("198.51.100.1"));
  EXPECT_FALSE(mp_is_public_ipv4("203.0.113.1"));
  EXPECT_FALSE(mp_is_public_ipv4("224.0.0.1"));
  EXPECT_FALSE(mp_is_public_ipv4("999.0.0.1"));
}

TEST(MultiplayerPortMapping, ProjectsHostInviteLifecycle) {
  MultiplayerData data;
  ASSERT_TRUE(data.security.start_host(26210));
  data.initialized = true;
  data.local_role = 0;
  EXPECT_EQ(multiplayer_host_invite_status(data), 1);

  data.internet_host = true;
  data.port_mapping_state = MPPortMappingState::PENDING;
  EXPECT_EQ(multiplayer_host_invite_status(data), 0);
  data.port_mapping_state = MPPortMappingState::FAILED;
  EXPECT_EQ(multiplayer_host_invite_status(data), -1);
  data.port_mapping_state = MPPortMappingState::READY;
  EXPECT_EQ(multiplayer_host_invite_status(data), 0);
  data.port_mapping_external_ip = "8.8.8.8";
  EXPECT_EQ(multiplayer_host_invite_status(data), 1);
}

TEST(MultiplayerDirectConnect, ValidatesOptionalTokenAndClearsDraft) {
  auto& data = multiplayer_data();
  pc_multi_reset_direct_connect();
  EXPECT_STREQ(data.direct_port.data(), "26210");
  for (const char character : std::string("25.1.2.3")) {
    EXPECT_EQ(pc_multi_edit_direct_field(0, static_cast<u32>(character)), 1);
  }
  EXPECT_EQ(pc_multi_direct_connect_ready(), 1);
  EXPECT_EQ(pc_multi_edit_direct_field(2, static_cast<u32>('/')), 0);
  const std::string token = "AAAAAAAAAAAAAAAAAAAAAA";
  EXPECT_EQ(pc_multi_edit_direct_field(2, static_cast<u32>(token[0])), 1);
  EXPECT_EQ(pc_multi_direct_connect_ready(), 0);
  for (const char character : token.substr(1)) {
    EXPECT_EQ(pc_multi_edit_direct_field(2, static_cast<u32>(character)), 1);
  }
  EXPECT_EQ(pc_multi_direct_connect_ready(), 1);
  pc_multi_clear_direct_connect();
  EXPECT_EQ(data.direct_address[0], '\0');
  EXPECT_EQ(data.direct_port[0], '\0');
  EXPECT_EQ(data.direct_token[0], '\0');
}

TEST(MultiplayerReconnect, StartsFromSavedInviteAndClearsOnDisconnect) {
  auto& data = multiplayer_data();
  pc_multi_disconnect();

  MultiplayerSecurity host;
  ASSERT_TRUE(host.start_host(26210));
  data.local_version = "v1.0.0";
  data.reconnect_invite = host.invite_for_address("127.0.0.1");

  EXPECT_EQ(pc_multi_reconnect(), 1);
  EXPECT_TRUE(data.initialized);
  EXPECT_EQ(data.local_role, 1);
  EXPECT_FALSE(data.reconnect_invite.empty());

  pc_multi_disconnect();
  EXPECT_TRUE(data.reconnect_invite.empty());
}

TEST(MultiplayerReconnect, MissingInviteFailsWithoutStartingClient) {
  auto& data = multiplayer_data();
  pc_multi_disconnect();

  EXPECT_EQ(pc_multi_reconnect(), 0);
  EXPECT_FALSE(data.initialized);
  EXPECT_EQ(data.join_status.load(), static_cast<int>(MultiplayerStatus::FAILED));
}

TEST(MultiplayerSecurity, RejectsMalformedAndNonIpv4Invites) {
  MultiplayerSecurity client;
  std::string host;
  uint16_t port = 0;
  EXPECT_FALSE(client.start_client("127.0.0.1:26210", host, port));
  EXPECT_FALSE(client.start_client("ogmp://127.0.0.1:26210/AAAAAAAAAAAAAAAAAAAAAA", host, port));
  EXPECT_FALSE(client.start_client("example.com:26210/AAAAAAAAAAAAAAAAAAAAAA", host, port));
  EXPECT_FALSE(client.start_client("999.0.0.1:26210/AAAAAAAAAAAAAAAAAAAAAA", host, port));
  EXPECT_FALSE(client.start_client("127.0.0.1:0/AAAAAAAAAAAAAAAAAAAAAA", host, port));
  EXPECT_FALSE(client.start_client("127.0.0.1:26210/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                                   host, port));
  EXPECT_FALSE(client.start_client("127.0.0.1:26210/AAAAAAAAAAAAAAAAAAAAAA/trailing", host, port));
}

TEST(MultiplayerSecurity, EncryptsAuthenticatesAndRejectsReplay) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(host.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
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

TEST(MultiplayerSecurity, RotatesHostPeerSessionAndPreservesInvite) {
  MultiplayerSecurity host;
  MultiplayerSecurity first_client;
  ASSERT_TRUE(host.start_host(26210));
  const std::string invite = host.invite_for_address("127.0.0.1");
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(first_client.start_client(invite, parsed_host, parsed_port));
  authenticate(host, first_client);

  PacketHeader old_header = {PacketType::EVENT_GAME, 1};
  MultiplayerDatagram old_ciphertext;
  ASSERT_TRUE(
      first_client.seal(1, old_header.type, &old_header, sizeof(old_header), old_ciphertext));

  ASSERT_TRUE(host.rotate_host_peer_session());
  EXPECT_FALSE(host.authenticated());
  EXPECT_TRUE(host.remote_version().empty());
  EXPECT_EQ(host.invite_for_address("127.0.0.1"), invite);
  EXPECT_EQ(host.receive(0, old_ciphertext.bytes.data(), old_ciphertext.size).kind,
            SecurityReceiveKind::REJECTED);

  MultiplayerSecurity replacement_client;
  ASSERT_TRUE(replacement_client.start_client(invite, parsed_host, parsed_port));
  authenticate(host, replacement_client);

  PacketHeader new_header = {PacketType::EVENT_GAME, 2};
  MultiplayerDatagram new_ciphertext;
  ASSERT_TRUE(
      replacement_client.seal(1, new_header.type, &new_header, sizeof(new_header), new_ciphertext));
  EXPECT_EQ(host.receive(0, new_ciphertext.bytes.data(), new_ciphertext.size).kind,
            SecurityReceiveKind::GAMEPLAY);
}

TEST(MultiplayerSession, ClearsOnlyRemotePeerStateForActiveHost) {
  MultiplayerData data;
  MultiplayerSecurity client;
  ASSERT_TRUE(data.security.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(
      client.start_client(data.security.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
  authenticate(data.security, client);
  const std::string invite_token = data.security.invite_token();
  data.local_role = 0;
  data.join_status = (int)MultiplayerStatus::CONNECTED_LOBBY;
  data.local_version = "dev-366c9e277";
  data.staged_invite = "private-staged-value";
  data.internet_host = true;
  multiplayer_set_status(data, (int)MultiplayerStatus::IN_GAME);
  ASSERT_TRUE(data.host_game_active);
  ASSERT_TRUE(data.pending_full_sync);

  data.remote_entity.last_sequence_num = 42;
  data.remote_entity.riding = 1;
  data.last_receive_time = 1234;
  data.last_enemy_sequence = 9;
  data.remote_enemy_buffer.remote_enemies[0].actor_id = 7;
  data.traffic_buffer.vehicles[0].net_id = 11;
  PacketGameEvent event = {};
  event.header.type = PacketType::EVENT_GAME;
  data.inbound_events.push_overwrite(event);

  ASSERT_TRUE(multiplayer_prepare_host_for_next_peer(data));

  EXPECT_TRUE(data.host_game_active);
  EXPECT_EQ(data.join_status, (int)MultiplayerStatus::IN_GAME);
  EXPECT_FALSE(data.security.authenticated());
  EXPECT_EQ(data.security.invite_token(), invite_token);
  EXPECT_EQ(data.local_version, "dev-366c9e277");
  EXPECT_EQ(data.staged_invite, "private-staged-value");
  EXPECT_TRUE(data.internet_host);
  EXPECT_FALSE(data.pending_full_sync);
  EXPECT_EQ(data.remote_entity.last_sequence_num, 0u);
  EXPECT_EQ(data.remote_entity.riding, 0u);
  EXPECT_EQ(data.last_receive_time, 0u);
  EXPECT_EQ(data.last_enemy_sequence, 0u);
  EXPECT_EQ(data.remote_enemy_buffer.remote_enemies[0].actor_id, 0u);
  EXPECT_EQ(data.traffic_buffer.vehicles[0].net_id, 0u);
  EXPECT_TRUE(data.inbound_events.empty());
}

TEST(MultiplayerSession, ReturnsPregameHostToWaitingForPeer) {
  MultiplayerData data;
  ASSERT_TRUE(data.security.start_host(26210));
  ASSERT_TRUE(data.security.set_local_version("v1.0.0"));
  data.local_role = 0;
  data.join_status = (int)MultiplayerStatus::CONNECTED_LOBBY;

  ASSERT_TRUE(multiplayer_prepare_host_for_next_peer(data));

  EXPECT_FALSE(data.host_game_active);
  EXPECT_EQ(data.join_status, (int)MultiplayerStatus::CONNECTING);
  EXPECT_FALSE(data.security.authenticated());
}

TEST(MultiplayerSecurity, RejectsTamperingAndWrongInviteToken) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string invite = host.invite_for_address("127.0.0.1");
  const size_t token_offset = invite.find('/') + 1;
  invite[token_offset] = invite[token_offset] == 'A' ? 'B' : 'A';
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(invite, parsed_host, parsed_port));
  ASSERT_TRUE(host.set_local_version("v1.0.0"));
  ASSERT_TRUE(client.set_local_version("v1.0.0"));

  MultiplayerDatagram server_hello;
  ASSERT_TRUE(host.make_server_hello(server_hello));
  SecurityReceiveResult client_proof =
      client.receive(1, server_hello.bytes.data(), server_hello.size);
  ASSERT_GT(client_proof.response.size, 0);
  SecurityReceiveResult rejected =
      host.receive(0, client_proof.response.bytes.data(), client_proof.response.size);
  EXPECT_EQ(rejected.kind, SecurityReceiveKind::REJECTED);
  EXPECT_FALSE(host.authenticated());
}

TEST(MultiplayerVersion, AcceptsCanonicalReleaseAndDevelopmentIdentities) {
  std::string canonical;
  EXPECT_TRUE(mp_canonicalize_semver("v0.3.0-beta-rc-1.0", canonical));
  EXPECT_EQ(canonical, "v0.3.0-beta-rc-1.0");
  EXPECT_TRUE(mp_canonicalize_semver("1.0.1-test-3.1", canonical));
  EXPECT_EQ(canonical, "v1.0.1-test-3.1");
  EXPECT_TRUE(mp_canonicalize_semver("v4.1.4-guns-on-board.0+windows", canonical));
  EXPECT_EQ(canonical, "v4.1.4-guns-on-board.0+windows");

  EXPECT_TRUE(
      mp_resolve_compatibility_identity(kMultiplayerVersionPlaceholder, "366c9e277", canonical));
  EXPECT_EQ(canonical, "dev-366c9e277");
}

TEST(MultiplayerVersion, RejectsMalformedOrUnsafeIdentities) {
  std::string canonical;
  EXPECT_FALSE(mp_canonicalize_semver("v1.0", canonical));
  EXPECT_FALSE(mp_canonicalize_semver("v01.0.0", canonical));
  EXPECT_FALSE(mp_canonicalize_semver("v1.0.0-feature/foo", canonical));
  EXPECT_FALSE(mp_canonicalize_semver("v1.0.0-feature..one", canonical));
  EXPECT_FALSE(mp_canonicalize_semver("v1.0.0-01", canonical));
  EXPECT_FALSE(mp_canonicalize_semver("v1.0.0 trailing", canonical));
  EXPECT_FALSE(mp_canonicalize_semver(std::string(65, '1'), canonical));
  EXPECT_FALSE(
      mp_resolve_compatibility_identity(kMultiplayerVersionPlaceholder, "unknown", canonical));
}

TEST(MultiplayerSecurity, AuthenticatesVersionMismatchBeforeRejectingClient) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(host.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
  ASSERT_TRUE(host.set_local_version("v0.3.0-beta-rc-1.0"));
  ASSERT_TRUE(client.set_local_version("v0.3.0-beta-rc-1.1"));

  MultiplayerDatagram server_hello;
  ASSERT_TRUE(host.make_server_hello(server_hello));
  SecurityReceiveResult client_proof =
      client.receive(1, server_hello.bytes.data(), server_hello.size);
  ASSERT_EQ(client_proof.kind, SecurityReceiveKind::HANDSHAKE);

  SecurityReceiveResult mismatch =
      host.receive(0, client_proof.response.bytes.data(), client_proof.response.size);
  ASSERT_EQ(mismatch.kind, SecurityReceiveKind::VERSION_MISMATCH);
  EXPECT_FALSE(host.authenticated());

  SecurityReceiveResult client_result =
      client.receive(1, mismatch.response.bytes.data(), mismatch.response.size);
  EXPECT_EQ(client_result.kind, SecurityReceiveKind::VERSION_MISMATCH);
  EXPECT_EQ(client.remote_version(), "v0.3.0-beta-rc-1.0");
  EXPECT_FALSE(client.authenticated());

  mismatch.response.bytes[mismatch.response.size - 1] ^= 0x80;
  EXPECT_EQ(client.receive(1, mismatch.response.bytes.data(), mismatch.response.size).kind,
            SecurityReceiveKind::REJECTED);
}

TEST(MultiplayerSecurity, RejectsTruncatedVersionBeforeCopying) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(host.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
  ASSERT_TRUE(host.set_local_version("v1.0.0"));
  ASSERT_TRUE(client.set_local_version("v1.0.0"));

  MultiplayerDatagram hello;
  ASSERT_TRUE(host.make_server_hello(hello));
  constexpr size_t version_length_offset = 8 + 16 + 32;
  hello.bytes[version_length_offset] = 64;
  EXPECT_EQ(client.receive(1, hello.bytes.data(), hello.size).kind, SecurityReceiveKind::REJECTED);
}

TEST(MultiplayerSecurity, RejectsAlteredCiphertextAndAcceptsReordering) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(host.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
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
  EXPECT_EQ(client.receive(1, first.bytes.data(), first.size).kind, SecurityReceiveKind::REJECTED);
  EXPECT_EQ(client.receive(1, third.bytes.data(), third.size).kind, SecurityReceiveKind::GAMEPLAY);
  EXPECT_EQ(client.receive(1, second.bytes.data(), second.size).kind,
            SecurityReceiveKind::GAMEPLAY);
}
