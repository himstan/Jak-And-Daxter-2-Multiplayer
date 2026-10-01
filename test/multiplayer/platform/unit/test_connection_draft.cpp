#include <unordered_set>

#include "game/multiplayer/platform/session/connection_draft.h"
#include "game/multiplayer/platform/session/connection_input.h"
#include "gtest/gtest.h"

namespace {
using namespace multiplayer::platform;

ParticipantProfile profile() {
  return {.display_name = "Player", .character = PlayerCharacter::UNKNOWN};
}

TEST(ConnectionDraft, SetsValidatedFieldsAtomically) {
  ConnectionDraft draft;
  draft.reset(26212);
  EXPECT_EQ(draft.field(1), "26212");
  EXPECT_FALSE(draft.ready());
  EXPECT_FALSE(draft.set_field(0, "invalid"));
  EXPECT_TRUE(draft.field(0).empty());
  EXPECT_TRUE(draft.set_field(0, "127.0.0.1"));
  EXPECT_TRUE(draft.ready());
  EXPECT_FALSE(draft.set_field(1, "1023"));
  EXPECT_EQ(draft.field(1), "26212");
  EXPECT_TRUE(draft.set_field(2, "abc123"));
  EXPECT_EQ(draft.field(2), "ABC123");
  EXPECT_FALSE(draft.set_field(0, "invalid"));
  EXPECT_EQ(draft.field(0), "127.0.0.1");
  EXPECT_FALSE(draft.set_field(2, "ABC-12"));
  EXPECT_EQ(draft.field(2), "ABC123");
  EXPECT_FALSE(draft.set_field(-1, "127.0.0.2"));
  EXPECT_FALSE(draft.set_field(3, "127.0.0.2"));
  EXPECT_TRUE(draft.field(-1).empty());
  EXPECT_TRUE(draft.field(3).empty());
  EXPECT_TRUE(draft.ready());
  EXPECT_TRUE(draft.set_field(2, ""));
  EXPECT_TRUE(draft.field(2).empty());
}

TEST(ConnectionDraft, BuildsDirectedDiscoveryWithoutRoomCode) {
  ConnectionDraft draft;
  draft.reset(26212);
  ASSERT_TRUE(draft.set_field(0, "127.0.0.1"));
  DraftConnectionRequest request;
  ASSERT_TRUE(draft.build_direct(profile(), 26213, request));
  ASSERT_TRUE(request.discovery);
  EXPECT_FALSE(request.connection);
  EXPECT_EQ(request.discovery->directed_address, "127.0.0.1");
  EXPECT_EQ(request.discovery->expected_game_port, 26212);
  EXPECT_EQ(request.discovery->discovery_port, 26213);
  EXPECT_EQ(request.discovery->profile.display_name, "Player");
}

TEST(ConnectionDraft, BuildsCanonicalConnectionWithRoomCode) {
  ConnectionDraft draft;
  draft.reset(26212);
  ASSERT_TRUE(draft.set_field(0, "127.0.0.1"));
  ASSERT_TRUE(draft.set_field(2, "abc123"));
  DraftConnectionRequest request;
  ASSERT_TRUE(draft.build_direct(profile(), 26213, request));
  ASSERT_TRUE(request.connection);
  EXPECT_FALSE(request.discovery);
  EXPECT_EQ(request.connection->endpoint, "127.0.0.1");
  EXPECT_EQ(request.connection->port, 26212);
  EXPECT_EQ(request.connection->room_code, "ABC123");
}

TEST(ConnectionDraft, StagedInviteUsesThePlatformParser) {
  ConnectionDraft draft;
  EXPECT_FALSE(draft.stage("not-an-invite"));
  EXPECT_FALSE(draft.staged());
  ASSERT_TRUE(draft.stage("jadmp://10.0.0.5:26212/ROOM42"));
  EXPECT_TRUE(draft.staged());
  ControllerClientConfig request;
  ASSERT_TRUE(draft.build_staged(profile(), request));
  EXPECT_EQ(request.endpoint, "10.0.0.5");
  EXPECT_EQ(request.port, 26212);
  EXPECT_EQ(request.room_code, "ROOM42");
  EXPECT_EQ(request.local_profile.display_name, "Player");
  EXPECT_FALSE(draft.stage("not-an-invite"));
  auto next_profile = profile();
  next_profile.display_name = "NextPlayer";
  next_profile.game_extension = {7, 8};
  ASSERT_TRUE(draft.build_staged(next_profile, request));
  EXPECT_EQ(request.endpoint, "10.0.0.5");
  EXPECT_EQ(request.local_profile.display_name, "NextPlayer");
  EXPECT_EQ(request.local_profile.game_extension, next_profile.game_extension);
  draft.clear_staged();
  EXPECT_FALSE(draft.staged());
  EXPECT_FALSE(draft.build_staged(profile(), request));
}

TEST(ConnectionDraft, SharedConnectionInputPolicyRejectsInvalidValues) {
  uint16_t port = 0;
  EXPECT_FALSE(parse_network_port("1023", port));
  EXPECT_FALSE(parse_network_port(std::to_string(kMultiplayerDiscoveryPort), port));
  EXPECT_TRUE(parse_network_port("26212", port));
  EXPECT_EQ(port, 26212);
  EXPECT_FALSE(parse_network_port("65536", port));
  EXPECT_FALSE(parse_network_port("26212x", port));
  EXPECT_FALSE(parse_network_port("", port));
  EXPECT_EQ(port, 26212);
  EXPECT_TRUE(parse_network_port("1024", port));
  EXPECT_EQ(port, 1024);
  EXPECT_TRUE(parse_network_port("65535", port));
  EXPECT_EQ(port, 65535);

  std::string room_code;
  EXPECT_TRUE(normalize_room_code("abc123", room_code));
  EXPECT_EQ(room_code, "ABC123");
  EXPECT_FALSE(normalize_room_code("ABC-12", room_code));
  EXPECT_TRUE(room_code.empty());
  EXPECT_TRUE(normalize_room_code("", room_code));
  EXPECT_FALSE(normalize_room_code("", room_code, false));
}

TEST(ConnectionDraft, InvitationsPreserveTheirDistinctPortAndRoomCodeRules) {
  ConnectionDraft draft;
  draft.reset(26212);
  for (const auto port : {uint16_t{1}, kMultiplayerDiscoveryPort}) {
    EXPECT_FALSE(draft.set_field(1, std::to_string(port)));
    ASSERT_TRUE(draft.stage("jadmp://127.0.0.1:" + std::to_string(port) + "/ROOM42"));
    ControllerClientConfig request;
    ASSERT_TRUE(draft.build_staged(profile(), request));
    EXPECT_EQ(request.port, port);
  }
  EXPECT_FALSE(draft.stage("jadmp://127.0.0.1:0/ROOM42"));
  EXPECT_FALSE(draft.stage("jadmp://127.0.0.1:65536/ROOM42"));
  EXPECT_FALSE(draft.stage("jadmp://127.0.0.1:26212/room42"));
}

TEST(ConnectionInput, GeneratedRoomCodesHaveCanonicalFormat) {
  std::unordered_set<std::string> unique_codes;
  for (int i = 0; i < 20; ++i) {
    const auto code = generate_room_code();
    EXPECT_EQ(code.size(), kMultiplayerRoomCodeLength);
    std::string normalized;
    EXPECT_TRUE(normalize_room_code(code, normalized, false));
    EXPECT_EQ(code, normalized);
    unique_codes.insert(code);
  }
  EXPECT_GT(unique_codes.size(), 1u);
}
}  // namespace
