#include "game/multiplayer/platform/profile/profile_store.h"

#include <utility>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

#include "game/multiplayer/platform/core/limits.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <sys/file.h>
#endif

namespace multiplayer::platform {
namespace {
std::string lock_name(std::string game_id, const fs::path& root, const uint8_t slot) {
  for (auto& ch : game_id) {
    if (!std::isalnum(static_cast<unsigned char>(ch)))
      ch = '_';
  }
  uint64_t root_hash = 1469598103934665603ull;
  for (const unsigned char ch : root.lexically_normal().string()) {
    root_hash = (root_hash ^ ch) * 1099511628211ull;
  }
  return "Local\\OpenGOALMultiplayerProfile_" + game_id + "_" + std::to_string(root_hash) + "_" +
         std::to_string(slot);
}
}  // namespace

class ProfileLease::Impl {
 public:
  ~Impl() {
#ifdef _WIN32
    if (handle) {
      ReleaseMutex(handle);
      CloseHandle(handle);
    }
#else
    if (descriptor >= 0)
      close(descriptor);
#endif
  }
#ifdef _WIN32
  HANDLE handle = nullptr;
#else
  int descriptor = -1;
#endif
};

ProfileLease::ProfileLease(const uint8_t slot, fs::path directory, std::unique_ptr<Impl> impl)
    : slot_(slot), directory_(std::move(directory)), impl_(std::move(impl)) {}
ProfileLease::~ProfileLease() = default;
ProfileLease::ProfileLease(ProfileLease&&) noexcept = default;
ProfileLease& ProfileLease::operator=(ProfileLease&&) noexcept = default;

std::optional<ProfileLease> ProfileLease::acquire(const ProfileStorageConfig& config) {
  if (config.game_id.empty() || config.root_directory.empty() || config.maximum_instances == 0) {
    return std::nullopt;
  }
  try {
    file_util::create_dir_if_needed(config.root_directory);
    file_util::create_dir_if_needed(config.root_directory / config.game_id);
  } catch (const std::exception& error) {
    lg::error("[Multiplayer] Could not create profile root {}: {}", config.root_directory.string(),
              error.what());
    return std::nullopt;
  }
  for (uint16_t candidate = 1; candidate <= config.maximum_instances; ++candidate) {
    const auto slot = static_cast<uint8_t>(candidate);
    auto impl = std::make_unique<Impl>();
#ifdef _WIN32
    impl->handle =
        CreateMutexA(nullptr, true, lock_name(config.game_id, config.root_directory, slot).c_str());
    if (!impl->handle)
      continue;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(impl->handle);
      impl->handle = nullptr;
      continue;
    }
#else
    const auto lock_path =
        config.root_directory / config.game_id / ("profile-" + std::to_string(slot) + ".lock");
    impl->descriptor = open(lock_path.string().c_str(), O_RDWR | O_CREAT, 0600);
    if (impl->descriptor < 0 || flock(impl->descriptor, LOCK_EX | LOCK_NB) != 0)
      continue;
#endif
    auto directory = config.root_directory / config.game_id / ("profile-" + std::to_string(slot));
    try {
      file_util::create_dir_if_needed(directory);
    } catch (...) {
      continue;
    }
    return ProfileLease(slot, std::move(directory), std::move(impl));
  }
  return std::nullopt;
}

fs::path ProfileLease::common_profile_path() const {
  return directory_ / "identity.json";
}

fs::path ProfileLease::game_preferences_path() const {
  return directory_ / "preferences.json";
}

bool valid_display_name(const std::string_view name) {
  if (name.size() > kMaximumDisplayNameBytes)
    return false;
  for (const unsigned char ch : name) {
    if (ch < 0x20 || ch > 0x7e)
      return false;
  }
  return true;
}

bool load_player_profile(const ProfileLease& lease, StoredPlayerProfile& profile) {
  const auto path = lease.common_profile_path();
  lg::info("Loading profile from path: {}", path.string());
  if (!file_util::file_exists(path.string())) {
    profile = {};
    return true;
  }
  try {
    const auto root = parse_commented_json(file_util::read_text_file(path), path.string());
    if (!root.is_object() || !root.contains("display_name") ||
        !root.at("display_name").is_string() || !root.contains("preferred_character") ||
        !root.at("preferred_character").is_number_unsigned()) {
      return false;
    }
    StoredPlayerProfile parsed;
    parsed.display_name = root.at("display_name").get<std::string>();
    const auto character = root.at("preferred_character").get<uint32_t>();
    if (!valid_display_name(parsed.display_name) ||
        (character != static_cast<uint8_t>(PlayerCharacter::JAK) &&
         character != static_cast<uint8_t>(PlayerCharacter::DAXTER))) {
      return false;
    }
    parsed.preferred_character = static_cast<PlayerCharacter>(character);
    profile = std::move(parsed);
    return true;
  } catch (const std::exception& ex) {
    lg::error("[Multiplayer] Could not load player profile! {}", ex.what());
    return false;
  }
}

bool save_player_profile(const ProfileLease& lease, const StoredPlayerProfile& profile) {
  if (!valid_display_name(profile.display_name) ||
      (profile.preferred_character != PlayerCharacter::JAK &&
       profile.preferred_character != PlayerCharacter::DAXTER)) {
    return false;
  }
  try {
    json root;
    root["display_name"] = profile.display_name;
    root["preferred_character"] = static_cast<uint8_t>(profile.preferred_character);
    file_util::write_text_file(lease.common_profile_path(), root.dump(2));
    return true;
  } catch (const std::exception& error) {
    lg::error("[Multiplayer] Could not save player profile: {}", error.what());
    return false;
  }
}

}  // namespace multiplayer::platform
