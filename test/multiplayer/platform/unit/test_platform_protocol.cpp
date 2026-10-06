#include <array>
#include <chrono>

#include "common/util/FileUtil.h"

#include "game/multiplayer/platform/core/message_policy.h"
#include "game/multiplayer/platform/core/network_statistics.h"
#include "game/multiplayer/platform/discovery/invite.h"
#include "game/multiplayer/platform/profile/profile_store.h"
#include "game/multiplayer/platform/protocol/session_protocol.h"
#include "game/multiplayer/platform/session/cadence_scheduler.h"
#include "gtest/gtest.h"

namespace {

TEST(PlatformCore, GameplayPoliciesEnforceDirectionPayloadAndCadence) {
  constexpr std::array policies = {
      multiplayer::platform::GameMessagePolicy{
          1, "state", multiplayer::platform::MessageDirection::BIDIRECTIONAL,
          multiplayer::platform::Delivery::UNRELIABLE_REALTIME,
          multiplayer::platform::MessagePriority::CRITICAL,
          multiplayer::platform::CadenceMode::PERIODIC, 33, 128},
      multiplayer::platform::GameMessagePolicy{
          2, "world", multiplayer::platform::MessageDirection::HOST_TO_CLIENT,
          multiplayer::platform::Delivery::RELIABLE_ORDERED,
          multiplayer::platform::MessagePriority::NORMAL, multiplayer::platform::CadenceMode::DIRTY,
          0, 256},
  };
  ASSERT_TRUE(multiplayer::platform::validate_message_policies(policies, 256));
  EXPECT_TRUE(multiplayer::platform::can_send_message(policies[1],
                                                      multiplayer::platform::SessionRole::HOST));
  EXPECT_FALSE(multiplayer::platform::can_send_message(policies[1],
                                                       multiplayer::platform::SessionRole::CLIENT));
  EXPECT_EQ(multiplayer::platform::find_message_policy(policies, 1), &policies[0]);

  multiplayer::platform::CadenceScheduler cadence;
  EXPECT_TRUE(cadence.due(policies[0], 100));
  EXPECT_FALSE(cadence.due(policies[0], 132));
  EXPECT_TRUE(cadence.due(policies[0], 133));
  EXPECT_FALSE(cadence.due(policies[1], 200, false));
  EXPECT_TRUE(cadence.due(policies[1], 200, true));
  EXPECT_TRUE(cadence.due({3, "event", multiplayer::platform::MessageDirection::BIDIRECTIONAL,
                           multiplayer::platform::Delivery::RELIABLE_ORDERED,
                           multiplayer::platform::MessagePriority::NORMAL,
                           multiplayer::platform::CadenceMode::ON_DEMAND, 0, 1},
                          200));
  cadence.reset();
  EXPECT_TRUE(cadence.due(policies[0], 101));

  constexpr std::array duplicate = {
      policies[0], multiplayer::platform::GameMessagePolicy{
                       1, "duplicate", multiplayer::platform::MessageDirection::BIDIRECTIONAL,
                       multiplayer::platform::Delivery::UNRELIABLE_REALTIME,
                       multiplayer::platform::MessagePriority::NORMAL,
                       multiplayer::platform::CadenceMode::PERIODIC, 1, 64}};
  EXPECT_FALSE(multiplayer::platform::validate_message_policies(duplicate, 256));
}

TEST(PlatformCore, PressureClassificationHysteresisAndCadenceProtectCriticalState) {
  using namespace multiplayer::platform;
  AggregateSnapshot snapshot = {.send_bytes_per_second = 900.0f,
                                .send_rate_bytes_per_second = 1000,
                                .minimum_local_quality = 0.8f,
                                .worst_bulk_queue_time_us = 300000};
  EXPECT_EQ(classify_network_pressure(snapshot), NetworkPressure::CONGESTED);
  snapshot.minimum_local_quality = 0.4f;
  EXPECT_EQ(classify_network_pressure(snapshot), NetworkPressure::SEVERE);

  NetworkPressureTracker tracker;
  EXPECT_EQ(tracker.update(NetworkPressure::SEVERE, 100), NetworkPressure::NORMAL);
  EXPECT_EQ(tracker.update(NetworkPressure::SEVERE, 349), NetworkPressure::NORMAL);
  EXPECT_EQ(tracker.update(NetworkPressure::SEVERE, 350), NetworkPressure::SEVERE);
  EXPECT_EQ(tracker.severe_since_ms(), 350u);
  EXPECT_EQ(tracker.update(NetworkPressure::NORMAL, 400), NetworkPressure::SEVERE);
  EXPECT_EQ(tracker.update(NetworkPressure::NORMAL, 3399), NetworkPressure::SEVERE);
  EXPECT_EQ(tracker.update(NetworkPressure::NORMAL, 3400), NetworkPressure::NORMAL);

  tracker.reset();
  EXPECT_EQ(tracker.update(NetworkPressure::SEVERE, 0), NetworkPressure::NORMAL);
  EXPECT_EQ(tracker.update(NetworkPressure::SEVERE, 250), NetworkPressure::SEVERE);
  EXPECT_EQ(tracker.severe_since_ms(), 250u);

  constexpr GameMessagePolicy critical = {10,
                                          "critical",
                                          MessageDirection::BIDIRECTIONAL,
                                          Delivery::UNRELIABLE_REALTIME,
                                          MessagePriority::CRITICAL,
                                          CadenceMode::PERIODIC,
                                          100,
                                          64};
  constexpr GameMessagePolicy normal = {11,
                                        "normal",
                                        MessageDirection::BIDIRECTIONAL,
                                        Delivery::UNRELIABLE_REALTIME,
                                        MessagePriority::NORMAL,
                                        CadenceMode::PERIODIC,
                                        100,
                                        64};
  constexpr GameMessagePolicy bulk = {12,
                                      "bulk",
                                      MessageDirection::BIDIRECTIONAL,
                                      Delivery::UNRELIABLE_REALTIME,
                                      MessagePriority::BULK,
                                      CadenceMode::PERIODIC,
                                      100,
                                      64};
  CadenceScheduler cadence;
  EXPECT_TRUE(cadence.due(critical, 100, true, NetworkPressure::SEVERE));
  EXPECT_TRUE(cadence.due(normal, 100, true, NetworkPressure::SEVERE));
  EXPECT_TRUE(cadence.due(bulk, 100, true, NetworkPressure::SEVERE));
  EXPECT_TRUE(cadence.due(critical, 200, true, NetworkPressure::SEVERE));
  EXPECT_FALSE(cadence.due(normal, 200, true, NetworkPressure::SEVERE));
  EXPECT_FALSE(cadence.due(bulk, 599, true, NetworkPressure::SEVERE));
  EXPECT_TRUE(cadence.due(bulk, 600, true, NetworkPressure::SEVERE));

  constexpr std::array invalid = {
      GameMessagePolicy{13, "invalid", MessageDirection::BIDIRECTIONAL, Delivery::RELIABLE_ORDERED,
                        MessagePriority::CRITICAL, CadenceMode::ON_DEMAND, 0, 64}};
  EXPECT_FALSE(validate_message_policies(invalid, 64));
}

TEST(PlatformSession, CommonProtocolRoundTripsAndRejectsMalformedFrames) {
  multiplayer::platform::ControlMessage control = {
      .kind = multiplayer::platform::ControlKind::PROFILE,
      .profile = {.player_id = 2,
                  .display_name = "Player2",
                  .character = multiplayer::platform::PlayerCharacter::DAXTER,
                  .ready = true,
                  .game_extension = {1, 2, 3}}};
  const auto control_bytes = multiplayer::platform::encode_control_message(control, 16);
  multiplayer::platform::ControlMessage decoded;
  ASSERT_TRUE(multiplayer::platform::decode_control_message(control_bytes, 16, 8, decoded));
  EXPECT_EQ(decoded.profile.display_name, "Player2");
  EXPECT_EQ(decoded.profile.game_extension, std::vector<uint8_t>({1, 2, 3}));
  auto trailing = control_bytes;
  trailing.push_back(0);
  EXPECT_FALSE(multiplayer::platform::decode_control_message(trailing, 16, 8, decoded));

  const std::array<uint8_t, 3> payload = {4, 5, 6};
  const auto gameplay = multiplayer::platform::encode_gameplay_envelope(7, UINT32_MAX, payload);
  multiplayer::platform::GameplayEnvelope gameplay_decoded;
  ASSERT_TRUE(multiplayer::platform::decode_gameplay_envelope(gameplay, gameplay_decoded));
  EXPECT_EQ(gameplay_decoded.message_id, 7);
  EXPECT_EQ(gameplay_decoded.sequence, UINT32_MAX);
  EXPECT_EQ(std::vector<uint8_t>(gameplay_decoded.payload.begin(), gameplay_decoded.payload.end()),
            std::vector<uint8_t>(payload.begin(), payload.end()));
  const auto empty_gameplay = multiplayer::platform::encode_gameplay_envelope(8, 1, {});
  ASSERT_EQ(empty_gameplay.size(), 5u);
  ASSERT_TRUE(multiplayer::platform::decode_gameplay_envelope(empty_gameplay, gameplay_decoded));
  EXPECT_TRUE(gameplay_decoded.payload.empty());

  const auto bootstrap = multiplayer::platform::encode_bootstrap_envelope(42, payload);
  multiplayer::platform::BootstrapEnvelope bootstrap_decoded;
  ASSERT_TRUE(multiplayer::platform::decode_bootstrap_envelope(bootstrap, bootstrap_decoded));
  EXPECT_EQ(bootstrap_decoded.generation, 42u);
}

TEST(PlatformSession, GameplayEnvelopesRejectZeroSequences) {
  using namespace multiplayer::platform;
  const std::array<uint8_t, 1> payload = {0x42};
  EXPECT_TRUE(encode_gameplay_envelope(7, 0, payload).empty());
  EXPECT_TRUE(encode_gameplay_envelope(7, 0, {}).empty());
  GameplayEnvelope decoded;
  const std::array<uint8_t, 6> zero_sequence = {7, 0, 0, 0, 0, 0x42};
  EXPECT_FALSE(decode_gameplay_envelope(zero_sequence, decoded));
  EXPECT_FALSE(decode_gameplay_envelope(std::span(zero_sequence).first(5), decoded));
  for (const auto sequence : {uint32_t{1}, UINT32_MAX}) {
    const auto bytes = encode_gameplay_envelope(7, sequence, payload);
    ASSERT_TRUE(decode_gameplay_envelope(bytes, decoded));
    EXPECT_EQ(decoded.sequence, sequence);
  }
}

TEST(PlatformSession, PlayerPingsUseCompactHostMeasurementsAndRejectMalformedLists) {
  using namespace multiplayer::platform;
  const ControlMessage message = {.kind = ControlKind::PLAYER_PINGS,
                                  .player_pings = {0, 83, 145, kUnknownPlayerPing}};
  const auto bytes = encode_control_message(message, 16);
  EXPECT_EQ(bytes, (std::vector<uint8_t>{11, 4, 0, 0, 83, 0, 145, 0, 255, 255}));
  ControlMessage decoded;
  ASSERT_TRUE(decode_control_message(bytes, 16, 4, decoded));
  EXPECT_EQ(decoded.player_pings, message.player_pings);
  for (size_t size = 0; size < bytes.size(); ++size)
    EXPECT_FALSE(decode_control_message(std::span(bytes).first(size), 16, 4, decoded));
  auto trailing = bytes;
  trailing.push_back(0);
  EXPECT_FALSE(decode_control_message(trailing, 16, 4, decoded));
  EXPECT_FALSE(decode_control_message(bytes, 16, 3, decoded));
  EXPECT_FALSE(decode_control_message(bytes, 16, 5, decoded));
  EXPECT_TRUE(encode_control_message({.kind = ControlKind::PLAYER_PINGS}, 16).empty());
  EXPECT_TRUE(encode_control_message({.kind = ControlKind::PLAYER_PINGS, .player_pings = {0}}, 16)
                  .empty());
}

TEST(PlatformSession, AdmissionGateCodecsAreTypedAndRejectTruncation) {
  const multiplayer::platform::ClientGate request = {
      .game_id = "jak3", .compatibility_identity = "v1.2.3", .room_code = "ABC123"};
  const auto request_bytes = multiplayer::platform::encode_client_gate(request);
  multiplayer::platform::ClientGate decoded_request;
  ASSERT_TRUE(multiplayer::platform::decode_client_gate(request_bytes, decoded_request));
  EXPECT_EQ(decoded_request.game_id, "jak3");
  EXPECT_EQ(decoded_request.compatibility_identity, "v1.2.3");
  EXPECT_FALSE(multiplayer::platform::decode_client_gate(
      std::span(request_bytes).first(request_bytes.size() - 1), decoded_request));
  auto trailing_request = request_bytes;
  trailing_request.push_back(0);
  EXPECT_FALSE(multiplayer::platform::decode_client_gate(trailing_request, decoded_request));
  auto old_request = request_bytes;
  old_request.insert(old_request.begin() + 5, {1, 0, 0, 0});
  EXPECT_FALSE(multiplayer::platform::decode_client_gate(old_request, decoded_request));

  const multiplayer::platform::ServerGate accepted = {
      .accepted = true, .player_id = 3, .host_player_id = 0, .player_capacity = 8};
  const auto accepted_bytes = multiplayer::platform::encode_server_gate(accepted);
  EXPECT_EQ(accepted_bytes.size(), 8u);
  multiplayer::platform::ServerGate decoded_response;
  ASSERT_TRUE(multiplayer::platform::decode_server_gate(accepted_bytes, decoded_response));
  EXPECT_TRUE(decoded_response.accepted);
  EXPECT_EQ(decoded_response.player_id, 3);
  EXPECT_FALSE(multiplayer::platform::decode_server_gate(
      std::span(accepted_bytes).first(accepted_bytes.size() - 1), decoded_response));
  auto trailing_response = accepted_bytes;
  trailing_response.push_back(2);
  EXPECT_FALSE(multiplayer::platform::decode_server_gate(trailing_response, decoded_response));

  const auto rejected_bytes = multiplayer::platform::encode_server_gate(
      {.rejection = multiplayer::platform::RejectionReason::VERSION_MISMATCH,
       .required_identity = "v1.2.3"});
  ASSERT_TRUE(multiplayer::platform::decode_server_gate(rejected_bytes, decoded_response));
  EXPECT_FALSE(decoded_response.accepted);
  EXPECT_EQ(decoded_response.rejection, multiplayer::platform::RejectionReason::VERSION_MISMATCH);
  EXPECT_EQ(decoded_response.required_identity, "v1.2.3");
  auto old_rejection = rejected_bytes;
  old_rejection.insert(old_rejection.begin() + 6, {1, 0, 0, 0});
  EXPECT_FALSE(multiplayer::platform::decode_server_gate(old_rejection, decoded_response));
}

TEST(PlatformSession, EveryCommonControlMessageHasExplicitHostClientAuthority) {
  using multiplayer::platform::ControlKind;
  using multiplayer::platform::SessionRole;
  EXPECT_TRUE(multiplayer::platform::control_allowed_from(ControlKind::PROFILE, SessionRole::HOST));
  EXPECT_TRUE(
      multiplayer::platform::control_allowed_from(ControlKind::PROFILE, SessionRole::CLIENT));
  for (const auto kind :
       {ControlKind::ROSTER, ControlKind::DEPARTURE, ControlKind::START_COUNTDOWN,
        ControlKind::CANCEL_COUNTDOWN, ControlKind::START_GAME, ControlKind::SESSION_CLOSE,
        ControlKind::PLAYER_PINGS}) {
    EXPECT_TRUE(multiplayer::platform::control_allowed_from(kind, SessionRole::HOST));
    EXPECT_FALSE(multiplayer::platform::control_allowed_from(kind, SessionRole::CLIENT));
  }
  for (const auto kind :
       {ControlKind::SET_CHARACTER, ControlKind::SET_READY, ControlKind::BOOTSTRAP_ACK}) {
    EXPECT_FALSE(multiplayer::platform::control_allowed_from(kind, SessionRole::HOST));
    EXPECT_TRUE(multiplayer::platform::control_allowed_from(kind, SessionRole::CLIENT));
  }
}

TEST(PlatformDiscovery, SharedInvitesUseJadmpScheme) {
  const auto invite = multiplayer::platform::make_invite("127.0.0.1", 26212, "ABC123");
  EXPECT_EQ(invite, "jadmp://127.0.0.1:26212/ABC123");
  std::string address;
  std::string room;
  uint16_t port = 0;
  EXPECT_TRUE(multiplayer::platform::parse_invite(invite, address, port, room));
  EXPECT_FALSE(
      multiplayer::platform::parse_invite("jad2mp://127.0.0.1:26212/ABC123", address, port, room));
}

TEST(PlatformSession, ProfileLeasesAreIndependentPerGameAndPersistIdentity) {
  const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const fs::path root = fs::temp_directory_path() / ("jadmp-profile-test-" + unique);
  const multiplayer::platform::ProfileStorageConfig jak2 = {
      .game_id = "jak2", .root_directory = root, .maximum_instances = 2};
  const multiplayer::platform::ProfileStorageConfig jak3 = {
      .game_id = "jak3", .root_directory = root, .maximum_instances = 2};
  {
    auto first = multiplayer::platform::ProfileLease::acquire(jak2);
    auto second = multiplayer::platform::ProfileLease::acquire(jak2);
    auto other_game = multiplayer::platform::ProfileLease::acquire(jak3);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(other_game.has_value());
    EXPECT_NE(first->slot(), second->slot());
    EXPECT_EQ(first->slot(), other_game->slot());
    EXPECT_NE(first->directory(), other_game->directory());
    multiplayer::platform::StoredPlayerProfile default_profile;
    ASSERT_TRUE(multiplayer::platform::load_player_profile(*first, default_profile));
    EXPECT_EQ(default_profile.display_name, "");
    EXPECT_EQ(default_profile.preferred_character, multiplayer::platform::PlayerCharacter::JAK);
    file_util::write_text_file(first->common_profile_path(),
                               R"({"display_name":"","preferred_character":1})");
    ASSERT_TRUE(multiplayer::platform::load_player_profile(*first, default_profile));
    EXPECT_EQ(default_profile.display_name, "");
    EXPECT_TRUE(multiplayer::platform::save_player_profile(
        *first, {.display_name = "Player1",
                 .preferred_character = multiplayer::platform::PlayerCharacter::DAXTER}));
    multiplayer::platform::StoredPlayerProfile loaded;
    ASSERT_TRUE(multiplayer::platform::load_player_profile(*first, loaded));
    EXPECT_EQ(loaded.display_name, "Player1");
    EXPECT_EQ(loaded.preferred_character, multiplayer::platform::PlayerCharacter::DAXTER);
    EXPECT_FALSE(multiplayer::platform::save_player_profile(
        *first, {.display_name = "1234567890123456",
                 .preferred_character = multiplayer::platform::PlayerCharacter::JAK}));
    EXPECT_FALSE(multiplayer::platform::save_player_profile(
        *first, {.display_name = "Player",
                 .preferred_character = multiplayer::platform::PlayerCharacter::UNKNOWN}));
    EXPECT_TRUE(multiplayer::platform::save_player_profile(
        *first,
        {.display_name = "", .preferred_character = multiplayer::platform::PlayerCharacter::JAK}));
  }
  fs::remove_all(root);
}

}  // namespace
