#include <limits>

#include "common/util/json_util.h"

#include "game/multiplayer/jak2/api/player_data_store.h"
#include "game/multiplayer/jak2/wire/packets/bootstrap_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/player_data_packet.h"
#include "gtest/gtest.h"

namespace {
using namespace multiplayer;

class PlayerDataTest : public testing::Test {
 protected:
  const fs::path directory = fs::temp_directory_path() /
                             ("jadmp-player-data-" + platform::PlayerId::generate().to_string());
  const platform::PlayerId first = platform::PlayerId::generate();
  const platform::PlayerId second = platform::PlayerId::generate();
  const jak2::core::PlayerData player_data = {
      .health = 1.5f,
      .dark_eco = 23.75f,
      .ammo = {2.0f, 0.0f, 3.5f, 4.0f},
      .cosmetic_secrets = 1u << 16,
      .unlocked_cosmetic_secrets = jak2::core::kPlayerCosmeticSecretsMask};

  void TearDown() override {
    std::error_code ignored;
    fs::remove(directory / "player-data.json.tmp", ignored);
    fs::remove(directory / "player-data.json", ignored);
    fs::remove(directory, ignored);
  }
};

TEST_F(PlayerDataTest, CapturesImmutableSnapshotAndSeparatesSaveSlots) {
  jak2::PlayerDataStore store(directory);
  store.update(first, player_data);
  ASSERT_TRUE(store.stage_save(0));
  auto later = player_data;
  later.health = 0;
  later.ammo = {};
  later.cosmetic_secrets = 1u << 17;
  later.unlocked_cosmetic_secrets = 1u << 17;
  store.update(first, later);
  store.update(second, later);
  EXPECT_FALSE(fs::exists(directory / "player-data.json"));
  EXPECT_FALSE(store.commit_save(1));
  ASSERT_TRUE(store.commit_save(0));
  EXPECT_EQ(store.get(first), later);
  ASSERT_TRUE(store.stage_save(1));
  ASSERT_TRUE(store.commit_save(1));

  jak2::PlayerDataStore restarted(directory);
  restarted.load(0);
  EXPECT_EQ(restarted.get(first), player_data);
  EXPECT_FALSE(restarted.get(second));
  restarted.load(1);
  EXPECT_EQ(restarted.get(first), later);
  EXPECT_EQ(restarted.get(second), later);
  restarted.load(0xff);
  EXPECT_FALSE(restarted.get(first));
}

TEST_F(PlayerDataTest, InvalidDataDoesNotReplaceLiveData) {
  jak2::PlayerDataStore store;
  store.update(first, player_data);
  auto invalid = player_data;
  invalid.ammo[1] = -1;
  store.update(first, invalid);
  invalid = player_data;
  invalid.dark_eco = std::numeric_limits<float>::quiet_NaN();
  store.update(first, invalid);
  store.update({}, player_data);
  EXPECT_EQ(store.get(first), player_data);
}

TEST_F(PlayerDataTest, LoadDiscardsPendingSaveAndUnsavedChanges) {
  jak2::PlayerDataStore store(directory);
  store.update(first, player_data);
  ASSERT_TRUE(store.stage_save(2));
  ASSERT_TRUE(store.commit_save(2));
  store.update(second, player_data);
  ASSERT_TRUE(store.stage_save(0));
  store.load(2);
  EXPECT_FALSE(store.commit_save(0));
  EXPECT_EQ(store.get(first), player_data);
  EXPECT_FALSE(store.get(second));
}

TEST_F(PlayerDataTest, PreservesCustomFieldsAndLoadsOlderPartialRecords) {
  file_util::create_dir_if_needed(directory);
  json root = {{"0",
                {{first.to_string(), {{"health", 2}, {"custom_mode", {{"score", 42}}}}},
                 {second.to_string(), {{"health", "broken"}}}}}};
  file_util::write_text_file(directory / "player-data.json", root.dump());
  jak2::PlayerDataStore store(directory);
  store.load(0);
  ASSERT_TRUE(store.get(first));
  EXPECT_EQ(store.get(first)->dark_eco, 0);
  EXPECT_EQ(store.get(first)->cosmetic_secrets, 0);
  EXPECT_EQ(store.get(first)->unlocked_cosmetic_secrets, 0);
  EXPECT_FALSE(store.get(second));
  store.update(first, player_data);
  ASSERT_TRUE(store.stage_save(0));
  ASSERT_TRUE(store.commit_save(0));
  root = json::parse(file_util::read_text_file(directory / "player-data.json"));
  EXPECT_EQ(root["0"][first.to_string()]["custom_mode"]["score"], 42);
}

TEST_F(PlayerDataTest, FailedWriteDoesNotAdvanceSavedSnapshot) {
  jak2::PlayerDataStore store(directory);
  store.update(first, player_data);
  ASSERT_TRUE(store.stage_save(0));
  ASSERT_TRUE(store.commit_save(0));
  store.update(second, player_data);
  ASSERT_TRUE(store.stage_save(0));
  fs::create_directory(directory / "player-data.json.tmp");
  EXPECT_FALSE(store.commit_save(0));
  store.load(0);
  EXPECT_FALSE(store.get(second));
}

TEST_F(PlayerDataTest, CosmeticFlagsUseOneByteAndRejectGameplayAndWorldSecrets) {
  jak2::wire::PlayerDataPacket packet;
  packet.bootstrap_generation = 1;
  packet.data = player_data;
  for (uint8_t bits = 0; bits < 16; ++bits) {
    packet.data.cosmetic_secrets = (bits & 3u) | ((bits & 12u) << 14);
    packet.data.unlocked_cosmetic_secrets =
        jak2::core::kPlayerCosmeticSecretsMask ^ packet.data.cosmetic_secrets;
    const auto bytes = platform::wire::encode_packet(packet);
    ASSERT_TRUE(bytes);
    ASSERT_EQ(bytes->size(), 29);
    EXPECT_EQ(bytes->back(), bits | ((15u ^ bits) << 4));
    const auto decoded = platform::wire::decode_packet<jak2::wire::PlayerDataPacket>(*bytes);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->data, packet.data);
  }
  for (uint32_t invalid : {1u << 2, 1u << 3, 1u << 4, 1u << 15, 1u << 31}) {
    packet.data = player_data;
    packet.data.cosmetic_secrets = invalid;
    EXPECT_FALSE(platform::wire::encode_packet(packet));
    jak2::PlayerDataStore store;
    store.update(first, packet.data);
    EXPECT_FALSE(store.get(first));
    packet.data = player_data;
    packet.data.unlocked_cosmetic_secrets = invalid;
    EXPECT_FALSE(platform::wire::encode_packet(packet));
  }
}

TEST_F(PlayerDataTest, DedicatedPacketAndOptionalBootstrapValidateData) {
  jak2::wire::PlayerDataPacket update;
  update.bootstrap_generation = 7;
  update.data = player_data;
  auto encoded = platform::wire::encode_packet(update);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded->size(), 29);
  const auto decoded = platform::wire::decode_packet<jak2::wire::PlayerDataPacket>(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, update);
  encoded->pop_back();
  EXPECT_FALSE(platform::wire::decode_packet<jak2::wire::PlayerDataPacket>(*encoded));
  update.bootstrap_generation = 0;
  EXPECT_FALSE(platform::wire::encode_packet(update));
  update.bootstrap_generation = 7;

  jak2::wire::BootstrapStatePacket bootstrap;
  bootstrap.player_data = player_data;
  encoded = platform::wire::encode_packet(bootstrap);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(platform::wire::decode_packet<jak2::wire::BootstrapStatePacket>(*encoded)->player_data,
            player_data);
  for (float invalid :
       {-1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
    update.data.health = invalid;
    bootstrap.player_data->health = invalid;
    EXPECT_FALSE(platform::wire::encode_packet(update));
    EXPECT_FALSE(platform::wire::encode_packet(bootstrap));
  }
}
}  // namespace
