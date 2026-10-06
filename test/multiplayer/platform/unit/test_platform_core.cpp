#include <thread>

#include "game/multiplayer/platform/core/compatibility_identity.h"
#include "game/multiplayer/platform/core/limits.h"
#include "game/multiplayer/platform/core/mailbox.h"
#include "game/multiplayer/platform/core/network_statistics.h"
#include "game/multiplayer/platform/core/sequence.h"
#include "game/multiplayer/platform/discovery/discovery_protocol.h"
#include "game/multiplayer/platform/discovery/discovery_service.h"
#include "game/multiplayer/platform/nat/port_mapping_coordinator.h"
#include "game/multiplayer/platform/protocol/message_frame.h"
#include "game/multiplayer/platform/runtime/multiplayer_runtime.h"
#include "game/multiplayer/platform/session/player_registry.h"
#include "game/multiplayer/platform/session/reconnect_policy.h"
#include "game/multiplayer/platform/session/session_snapshot.h"
#include "game/multiplayer/platform/session/session_state.h"
#include "gtest/gtest.h"

namespace {

TEST(PlatformCore, CompatibilityIdentityCanonicalizesReleaseAndDevelopmentBuilds) {
  std::string identity;
  EXPECT_TRUE(multiplayer::platform::canonicalize_semver("1.2.3-beta.1+build", identity));
  EXPECT_EQ(identity, "v1.2.3-beta.1+build");
  EXPECT_TRUE(multiplayer::platform::valid_compatibility_identity(identity));
  EXPECT_TRUE(multiplayer::platform::resolve_compatibility_identity(
      multiplayer::platform::kCompatibilityIdentityPlaceholder, "abcdef1", identity));
  EXPECT_EQ(identity, "dev-abcdef1");
}

TEST(PlatformCore, CompatibilityIdentityRejectsMalformedValues) {
  std::string identity;
  EXPECT_FALSE(multiplayer::platform::canonicalize_semver("1.2", identity));
  EXPECT_FALSE(multiplayer::platform::canonicalize_semver("1.02.3", identity));
  EXPECT_FALSE(multiplayer::platform::valid_compatibility_identity("dev-xyz"));
  EXPECT_FALSE(multiplayer::platform::resolve_compatibility_identity(
      multiplayer::platform::kCompatibilityIdentityPlaceholder, "123", identity));
}

TEST(PlatformCore, SequenceComparisonHandlesEqualityStalenessAndWraparound) {
  EXPECT_TRUE(multiplayer::platform::sequence_is_newer(1, 0));
  EXPECT_TRUE(multiplayer::platform::sequence_is_newer(0, UINT32_MAX));
  EXPECT_FALSE(multiplayer::platform::sequence_is_newer(9, 10));
  EXPECT_TRUE(multiplayer::platform::sequence_is_current_or_newer(10, 10));
  uint32_t sequence = UINT32_MAX;
  EXPECT_EQ(multiplayer::platform::advance_nonzero_sequence(sequence), 1u);
}

TEST(PlatformCore, LatestValueMailboxOverwritesTakesAndClears) {
  multiplayer::platform::LatestValueMailbox<int> mailbox;
  mailbox.publish(1);
  mailbox.publish(2);
  ASSERT_EQ(mailbox.take(), 2);
  EXPECT_FALSE(mailbox.take());
  mailbox.publish(3);
  mailbox.clear();
  EXPECT_FALSE(mailbox.take());
}

TEST(PlatformCore, BoundedMailboxPreservesOrderAndAtomicBatches) {
  multiplayer::platform::BoundedMailbox<int, 5> mailbox;
  const std::array back = {3, 4};
  const std::array front = {1, 2};
  ASSERT_TRUE(mailbox.push_back(back));
  ASSERT_TRUE(mailbox.push_front(front));
  EXPECT_EQ(mailbox.size(), 4u);
  EXPECT_EQ(mailbox.remaining(), 1u);
  const std::array overflow = {5, 6};
  EXPECT_FALSE(mailbox.push_back(overflow));
  EXPECT_EQ(mailbox.take(3), (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(mailbox.take(3), (std::vector<int>{4}));
  mailbox.clear();
  EXPECT_EQ(mailbox.size(), 0u);
}

TEST(PlatformCore, BoundedMailboxSelectiveRemovalPreservesOrderAndRestoresCapacity) {
  multiplayer::platform::BoundedMailbox<int, 5> mailbox;
  ASSERT_TRUE(mailbox.push_back(std::array{1, 2, 3, 4, 5}));
  EXPECT_EQ(mailbox.erase_if([](int value) { return value % 2 == 0; }), 2u);
  EXPECT_EQ(mailbox.remaining(), 2u);
  ASSERT_TRUE(mailbox.push_back(std::array{6, 7}));
  EXPECT_EQ(mailbox.take(5), (std::vector<int>{1, 3, 5, 6, 7}));
}

TEST(PlatformCore, BoundedMailboxConcurrentBatchesNeverInterleave) {
  multiplayer::platform::BoundedMailbox<int, 512> mailbox;
  const auto producer = [&](const int value) {
    const std::array batch = {value, value};
    for (size_t count = 0; count < 100; ++count)
      ASSERT_TRUE(mailbox.push_back(batch));
  };
  std::thread first(producer, 1);
  std::thread second(producer, 2);
  first.join();
  second.join();
  const auto values = mailbox.take(512);
  ASSERT_EQ(values.size(), 400u);
  for (size_t index = 0; index < values.size(); index += 2)
    EXPECT_EQ(values[index], values[index + 1]);
}

TEST(PlatformCore, NetworkStatisticsAggregateCapacityQueuesAndWorstHealth) {
  const auto idle = multiplayer::platform::aggregate_connection_snapshots({});
  EXPECT_FLOAT_EQ(idle.minimum_local_quality, -1.0f);
  EXPECT_FLOAT_EQ(idle.minimum_remote_quality, -1.0f);
  EXPECT_EQ(idle.send_rate_bytes_per_second, 0);

  const std::array<multiplayer::platform::ConnectionSnapshot, 2> connections = {{
      {.connection_id = 10,
       .ping_ms = 24,
       .jitter_us = 1500,
       .local_quality = 0.99f,
       .remote_quality = -1.0f,
       .send_bytes_per_second = 1024.0f,
       .receive_bytes_per_second = 2048.0f,
       .send_rate_bytes_per_second = 8192,
       .pending_unreliable_bytes = 3,
       .pending_reliable_bytes = 5,
       .sent_unacked_reliable_bytes = 7},
      {.connection_id = 11,
       .ping_ms = 61,
       .jitter_us = 9000,
       .local_quality = 0.82f,
       .remote_quality = 0.91f,
       .send_bytes_per_second = 4096.0f,
       .receive_bytes_per_second = 8192.0f,
       .send_rate_bytes_per_second = 16384,
       .pending_unreliable_bytes = 11,
       .pending_reliable_bytes = 13,
       .sent_unacked_reliable_bytes = 17},
  }};
  const auto aggregate = multiplayer::platform::aggregate_connection_snapshots(connections);
  EXPECT_FLOAT_EQ(aggregate.send_bytes_per_second, 5120.0f);
  EXPECT_FLOAT_EQ(aggregate.receive_bytes_per_second, 10240.0f);
  EXPECT_EQ(aggregate.send_rate_bytes_per_second, 24576);
  EXPECT_EQ(aggregate.pending_unreliable_bytes, 14);
  EXPECT_EQ(aggregate.pending_reliable_bytes, 18);
  EXPECT_EQ(aggregate.sent_unacked_reliable_bytes, 24);
  EXPECT_EQ(aggregate.worst_ping_ms, 61);
  EXPECT_EQ(aggregate.worst_jitter_us, 9000);
  EXPECT_FLOAT_EQ(aggregate.minimum_local_quality, 0.82f);
  EXPECT_FLOAT_EQ(aggregate.minimum_remote_quality, 0.91f);
  EXPECT_EQ(multiplayer::platform::kReliableBacklogLimitBytes, 384 * 1024);
}

TEST(PlatformSession, PlayerPingIsRelativeToTheHostForEveryViewer) {
  using namespace multiplayer::platform;
  SessionSnapshot snapshot;
  EXPECT_FALSE(snapshot.player_ping_ms(kInvalidPlayerId));
  EXPECT_FALSE(snapshot.player_ping_ms(0));

  snapshot.state.role = SessionRole::HOST;
  snapshot.state.local_player_id = 0;
  snapshot.state.host_player_id = 0;
  snapshot.player_pings = {0, 83, 145, kUnknownPlayerPing};
  EXPECT_EQ(snapshot.player_ping_ms(0), 0);
  EXPECT_EQ(snapshot.player_ping_ms(1), 83);
  EXPECT_EQ(snapshot.player_ping_ms(2), 145);
  EXPECT_FALSE(snapshot.player_ping_ms(3));
  EXPECT_FALSE(snapshot.player_ping_ms(4));
  EXPECT_FALSE(snapshot.player_ping_ms(kInvalidPlayerId));

  snapshot.state.role = SessionRole::CLIENT;
  snapshot.state.local_player_id = 1;
  snapshot.connections = {{.player_id = 0, .network = {.connection_id = 10, .ping_ms = 83}}};
  EXPECT_EQ(snapshot.player_ping_ms(0), 0);
  EXPECT_EQ(snapshot.player_ping_ms(1), 83);
  EXPECT_EQ(snapshot.player_ping_ms(2), 145);
  snapshot.state.local_player_id = 2;
  EXPECT_EQ(snapshot.player_ping_ms(1), 83);
  EXPECT_EQ(snapshot.player_ping_ms(2), 145);
  snapshot.player_pings.clear();
  EXPECT_FALSE(snapshot.player_ping_ms(2));
}

TEST(PlatformSession, MessageFrameKeepsAuthenticatedOriginOutsideGameplayPayload) {
  const std::vector<uint8_t> payload = {9, 8, 7};
  const auto bytes = multiplayer::platform::encode_message_frame(
      multiplayer::platform::FrameKind::GAMEPLAY, 2, payload);
  multiplayer::platform::MessageFrame frame;
  ASSERT_TRUE(multiplayer::platform::decode_message_frame(bytes, frame));
  EXPECT_EQ(frame.origin, 2u);
  EXPECT_EQ(std::vector<uint8_t>(frame.payload.begin(), frame.payload.end()), payload);
  EXPECT_FALSE(multiplayer::platform::decode_message_frame({bytes.data(), 3}, frame));
}

TEST(PlatformDiscovery, AdvertisementRoundTripsAndRejectsMalformedInput) {
  const multiplayer::platform::DiscoveryAdvertisement expected = {
      .game_port = 26212,
      .current_players = 2,
      .player_limit = 8,
      .game_id = "jak2",
      .compatibility_identity = "v1.2.3",
      .room_code = "ABC123"};
  const auto bytes = multiplayer::platform::encode_discovery_advertisement(expected);
  multiplayer::platform::DiscoveryAdvertisement decoded;
  ASSERT_TRUE(multiplayer::platform::decode_discovery_advertisement(bytes, decoded));
  EXPECT_EQ(decoded.game_id, expected.game_id);
  EXPECT_EQ(decoded.compatibility_identity, expected.compatibility_identity);
  EXPECT_EQ(decoded.room_code, expected.room_code);
  EXPECT_EQ(decoded.game_port, expected.game_port);
  EXPECT_FALSE(multiplayer::platform::decode_discovery_advertisement(
      {bytes.data(), bytes.size() - 1}, decoded));
  auto trailing = bytes;
  trailing.push_back(0);
  EXPECT_FALSE(multiplayer::platform::decode_discovery_advertisement(trailing, decoded));
  auto old_layout = bytes;
  old_layout.insert(old_layout.begin() + 6, {1, 0, 0, 0});
  EXPECT_FALSE(multiplayer::platform::decode_discovery_advertisement(old_layout, decoded));
  auto invalid = expected;
  invalid.current_players = 9;
  EXPECT_TRUE(multiplayer::platform::encode_discovery_advertisement(invalid).empty());
  invalid = expected;
  invalid.player_limit = 1;
  EXPECT_TRUE(multiplayer::platform::encode_discovery_advertisement(invalid).empty());
}

TEST(PlatformDiscovery, QueryCarriesTheRequestedGame) {
  const auto bytes = multiplayer::platform::encode_discovery_query("jak3");
  std::string game_id;
  EXPECT_TRUE(multiplayer::platform::decode_discovery_query(bytes, game_id));
  EXPECT_EQ(game_id, "jak3");
}

TEST(PlatformDiscovery, FilteringRejectsWrongIdentityGamePortAndCapacity) {
  const multiplayer::platform::DiscoveryConfig config = {.discovery_port = 26211,
                                                         .expected_game_port = 26210,
                                                         .game_id = "jak3",
                                                         .compatibility_identity = "v1.2.3",
                                                         .directed_address = "127.0.0.1"};
  multiplayer::platform::DiscoveryAdvertisement advertisement = {.game_port = 26210,
                                                                 .current_players = 1,
                                                                 .player_limit = 8,
                                                                 .game_id = "jak3",
                                                                 .compatibility_identity = "v1.2.3",
                                                                 .room_code = "ABC123"};
  EXPECT_TRUE(
      multiplayer::platform::discovery_advertisement_matches(config, advertisement, "127.0.0.1"));
  EXPECT_FALSE(
      multiplayer::platform::discovery_advertisement_matches(config, advertisement, "127.0.0.2"));
  auto lan = config;
  lan.directed_address.clear();
  EXPECT_TRUE(
      multiplayer::platform::discovery_advertisement_matches(lan, advertisement, "127.0.0.2"));
  advertisement.game_id = "jak2";
  EXPECT_FALSE(
      multiplayer::platform::discovery_advertisement_matches(config, advertisement, "127.0.0.1"));
  advertisement.game_id = "jak3";
  advertisement.compatibility_identity = "v1.2.4";
  EXPECT_FALSE(
      multiplayer::platform::discovery_advertisement_matches(config, advertisement, "127.0.0.1"));
  advertisement.compatibility_identity = "v1.2.3";
  ++advertisement.game_port;
  EXPECT_FALSE(
      multiplayer::platform::discovery_advertisement_matches(config, advertisement, "127.0.0.1"));
  advertisement.game_port = 26210;
  advertisement.current_players = advertisement.player_limit;
  EXPECT_FALSE(
      multiplayer::platform::discovery_advertisement_matches(config, advertisement, "127.0.0.1"));
  auto include_full = config;
  include_full.include_full_sessions = true;
  EXPECT_TRUE(multiplayer::platform::discovery_advertisement_matches(include_full, advertisement,
                                                                     "127.0.0.1"));
}

TEST(PlatformSession, PlayerRegistryAllocatesRejectsDuplicatesAndReusesSlots) {
  multiplayer::platform::PlayerRegistry registry(3);
  ASSERT_NE(registry.bind(10, 1, multiplayer::platform::PlayerCharacter::DAXTER), nullptr);
  ASSERT_NE(registry.bind(11, 2, multiplayer::platform::PlayerCharacter::JAK), nullptr);
  EXPECT_FALSE(registry.has_open_remote_slot());
  EXPECT_EQ(registry.bind(12, 1, multiplayer::platform::PlayerCharacter::JAK), nullptr);
  EXPECT_TRUE(registry.release_connection(10));
  EXPECT_TRUE(registry.has_open_remote_slot());
  ASSERT_NE(registry.bind(12, 1, multiplayer::platform::PlayerCharacter::JAK), nullptr);
  EXPECT_EQ(registry.find_player(1)->connection_id, 12u);
}

TEST(PlatformSession, BootstrapTrackingOnlyTargetsReadyPlayers) {
  multiplayer::platform::PlayerRegistry registry(3);
  auto* ready = registry.bind(10, 1, multiplayer::platform::PlayerCharacter::JAK);
  auto* pending_identity = registry.bind(11, 2, multiplayer::platform::PlayerCharacter::DAXTER);
  ASSERT_NE(ready, nullptr);
  ASSERT_NE(pending_identity, nullptr);
  ready->identity_ready = true;
  registry.request_bootstrap_for_all();
  EXPECT_TRUE(ready->bootstrap_pending);
  EXPECT_EQ(ready->bootstrap_generation, 1u);
  EXPECT_FALSE(pending_identity->bootstrap_pending);
  EXPECT_FALSE(registry.acknowledge_bootstrap(1, 2));
  EXPECT_TRUE(registry.acknowledge_bootstrap(1, 1));
  EXPECT_FALSE(registry.acknowledge_bootstrap(1, 1));
  registry.request_bootstrap_for_all();
  EXPECT_EQ(ready->bootstrap_generation, 2u);
  registry.request_bootstrap_for_all(42u);
  EXPECT_EQ(ready->bootstrap_generation, 42u);
  registry.reset();
  EXPECT_EQ(registry.accepted_count(), 0u);
}

TEST(PlatformSession, ReconnectPolicyUsesBoundedBackoffAcross32BitBoundary) {
  multiplayer::platform::ReconnectState reconnect;
  const uint64_t start = static_cast<uint64_t>(UINT32_MAX) - 100;
  multiplayer::platform::schedule_reconnect(reconnect, start);
  EXPECT_FALSE(multiplayer::platform::reconnect_due(reconnect, start + 249));
  EXPECT_TRUE(multiplayer::platform::reconnect_due(reconnect, start + 250));
  multiplayer::platform::mark_reconnect_attempt_started(reconnect);
  EXPECT_TRUE(reconnect.attempt_active);
  multiplayer::platform::mark_reconnect_attempt_failed(reconnect, 1000);
  EXPECT_EQ(reconnect.next_attempt_time_ms, 1500u);
  reconnect.attempt_count = 100;
  EXPECT_EQ(multiplayer::platform::reconnect_delay_ms(reconnect.attempt_count), 5000u);
  multiplayer::platform::reset_reconnect(reconnect);
  EXPECT_EQ(reconnect.next_attempt_time_ms, 0u);
}

TEST(PlatformSession, SessionStateRejectsAssignmentsOutsideConfiguredLimit) {
  multiplayer::platform::SessionState session = {.role = multiplayer::platform::SessionRole::CLIENT,
                                                 .player_limit = 4};
  multiplayer::platform::mark_connected(session, 3, 0);
  EXPECT_TRUE(multiplayer::platform::can_enter_game(session));
  multiplayer::platform::mark_connected(session, 4, 0);
  EXPECT_EQ(session.status, multiplayer::platform::SessionStatus::FAILED);
}

TEST(PlatformSession, ShutdownWithoutAdapterIsIdempotent) {
  multiplayer::platform::MultiplayerRuntime service;
  service.shutdown();
  service.shutdown();
  EXPECT_FALSE(service.active());
}

TEST(PlatformSession, PortMappingLifecycleStopsWithoutWaitingForNetworkWork) {
  multiplayer::platform::PortMappingCoordinator coordinator;
  EXPECT_EQ(coordinator.snapshot().state, multiplayer::platform::MPPortMappingState::IDLE);
  coordinator.start(26210, 26210);
  EXPECT_EQ(coordinator.snapshot().state, multiplayer::platform::MPPortMappingState::PENDING);
  coordinator.stop();
  EXPECT_EQ(coordinator.snapshot().state, multiplayer::platform::MPPortMappingState::IDLE);
  coordinator.stop();
}

}  // namespace
