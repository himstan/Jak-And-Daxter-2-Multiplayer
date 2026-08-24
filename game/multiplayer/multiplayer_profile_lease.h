#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "common/util/FileUtil.h"

class MultiplayerProfileLease {
 public:
  MultiplayerProfileLease() = delete;
  ~MultiplayerProfileLease();

  MultiplayerProfileLease(const MultiplayerProfileLease&) = delete;
  MultiplayerProfileLease& operator=(const MultiplayerProfileLease&) = delete;
  MultiplayerProfileLease(MultiplayerProfileLease&& other) noexcept;
  MultiplayerProfileLease& operator=(MultiplayerProfileLease&& other) noexcept;

  static std::optional<MultiplayerProfileLease> acquire(GameVersion game_version);

  uint32_t profile_id() const { return m_profile_id; }
  const fs::path& settings_path() const { return m_settings_path; }

 private:
  class Impl;

  MultiplayerProfileLease(uint32_t profile_id,
                          fs::path settings_path,
                          std::unique_ptr<Impl> implementation);

  uint32_t m_profile_id;
  fs::path m_settings_path;
  std::unique_ptr<Impl> m_impl;
};

fs::path mp_multiplayer_profile_directory(GameVersion game_version);
fs::path mp_multiplayer_profile_settings_path(GameVersion game_version, uint32_t profile_id);
