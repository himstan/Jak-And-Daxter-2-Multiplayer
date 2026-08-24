#include "game/multiplayer/multiplayer_profile_lease.h"

#include <exception>
#include <string>
#include <utility>

#include "common/log/log.h"
#include "common/versions/versions.h"

#include "game/multiplayer/multiplayer_protocol.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <sys/file.h>
#endif

namespace {
constexpr uint32_t kFirstProfileId = 1;

std::string profile_file_name(uint32_t profile_id, const char* extension) {
  return "profile-" + std::to_string(profile_id) + extension;
}

#ifdef _WIN32
std::string profile_mutex_name(GameVersion game_version, uint32_t profile_id) {
  return "Local\\OpenGOALMultiplayerProfile_" + version_to_game_name(game_version) + "_" +
         std::to_string(profile_id);
}
#else
fs::path profile_lock_path(GameVersion game_version, uint32_t profile_id) {
  return mp_multiplayer_profile_directory(game_version) / profile_file_name(profile_id, ".lock");
}
#endif
}  // namespace

class MultiplayerProfileLease::Impl {
 public:
  ~Impl() {
#ifdef _WIN32
    if (handle) {
      CloseHandle(handle);
    }
#else
    if (file_descriptor >= 0) {
      close(file_descriptor);
    }
#endif
  }

#ifdef _WIN32
  HANDLE handle = nullptr;
#else
  int file_descriptor = -1;
#endif
};

fs::path mp_multiplayer_profile_directory(GameVersion game_version) {
  return file_util::get_user_settings_dir(game_version) / "multiplayer-profiles";
}

fs::path mp_multiplayer_profile_settings_path(GameVersion game_version, uint32_t profile_id) {
  return mp_multiplayer_profile_directory(game_version) / profile_file_name(profile_id, ".json");
}

MultiplayerProfileLease::MultiplayerProfileLease(uint32_t profile_id,
                                                 fs::path settings_path,
                                                 std::unique_ptr<Impl> implementation)
    : m_profile_id(profile_id),
      m_settings_path(std::move(settings_path)),
      m_impl(std::move(implementation)) {}

MultiplayerProfileLease::~MultiplayerProfileLease() = default;

MultiplayerProfileLease::MultiplayerProfileLease(MultiplayerProfileLease&& other) noexcept
    : m_profile_id(other.m_profile_id),
      m_settings_path(std::move(other.m_settings_path)),
      m_impl(std::move(other.m_impl)) {
  other.m_profile_id = 0;
}

MultiplayerProfileLease& MultiplayerProfileLease::operator=(
    MultiplayerProfileLease&& other) noexcept {
  if (this != &other) {
    m_profile_id = other.m_profile_id;
    m_settings_path = std::move(other.m_settings_path);
    m_impl = std::move(other.m_impl);
    other.m_profile_id = 0;
  }
  return *this;
}

std::optional<MultiplayerProfileLease> MultiplayerProfileLease::acquire(GameVersion game_version) {
  const auto profile_directory = mp_multiplayer_profile_directory(game_version);
  try {
    file_util::create_dir_if_needed(profile_directory);
  } catch (const std::exception& error) {
    lg::error("[Multiplayer] Could not create profile directory {}: {}", profile_directory.string(),
              error.what());
    return std::nullopt;
  }

  for (uint32_t profile_id = kFirstProfileId; profile_id <= kMPMaxPlayers; ++profile_id) {
    auto implementation = std::make_unique<Impl>();
#ifdef _WIN32
    implementation->handle =
        CreateMutexA(nullptr, false, profile_mutex_name(game_version, profile_id).c_str());
    if (!implementation->handle) {
      lg::warn("[Multiplayer] Could not create profile {} mutex.", profile_id);
      continue;
    }

    const DWORD wait_result = WaitForSingleObject(implementation->handle, 0);
    if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
      continue;
    }
#else
    const auto lock_path = profile_lock_path(game_version, profile_id);
    implementation->file_descriptor = open(lock_path.string().c_str(), O_RDWR | O_CREAT, 0600);
    if (implementation->file_descriptor < 0) {
      lg::warn("[Multiplayer] Could not open profile {} lock file at {}.", profile_id,
               lock_path.string());
      continue;
    }
    if (flock(implementation->file_descriptor, LOCK_EX | LOCK_NB) != 0) {
      continue;
    }
#endif

    auto settings_path = mp_multiplayer_profile_settings_path(game_version, profile_id);
    lg::info("[Multiplayer] Assigned profile {} using {}.", profile_id, settings_path.string());
    return MultiplayerProfileLease(profile_id, std::move(settings_path), std::move(implementation));
  }

  lg::error("[Multiplayer] Could not acquire a multiplayer profile slot.");
  return std::nullopt;
}
