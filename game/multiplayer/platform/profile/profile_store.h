#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "common/util/FileUtil.h"

#include "game/multiplayer/platform/core/types.h"

namespace multiplayer::platform {

struct StoredPlayerProfile {
  std::string display_name;
  PlayerCharacter preferred_character = PlayerCharacter::JAK;
};

struct ProfileStorageConfig {
  std::string game_id;
  fs::path root_directory;
  uint8_t maximum_instances = 8;
};

class ProfileLease {
 public:
  ProfileLease() = delete;
  ~ProfileLease();
  ProfileLease(ProfileLease&&) noexcept;
  ProfileLease& operator=(ProfileLease&&) noexcept;
  ProfileLease(const ProfileLease&) = delete;
  ProfileLease& operator=(const ProfileLease&) = delete;

  static std::optional<ProfileLease> acquire(const ProfileStorageConfig& config);

  uint8_t slot() const { return slot_; }
  const fs::path& directory() const { return directory_; }
  fs::path common_profile_path() const;
  fs::path game_preferences_path() const;

 private:
  class Impl;
  ProfileLease(uint8_t slot, fs::path directory, std::unique_ptr<Impl> impl);

  uint8_t slot_ = 0;
  fs::path directory_;
  std::unique_ptr<Impl> impl_;
};

bool load_player_profile(const ProfileLease& lease, StoredPlayerProfile& profile);
bool save_player_profile(const ProfileLease& lease, const StoredPlayerProfile& profile);
bool valid_display_name(std::string_view name);

}  // namespace multiplayer::platform
