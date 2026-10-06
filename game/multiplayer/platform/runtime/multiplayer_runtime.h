#pragma once

#include <concepts>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "game/multiplayer/platform/discovery/discovery_service.h"
#include "game/multiplayer/platform/profile/profile_store.h"
#include "game/multiplayer/platform/runtime/runtime_command.h"
#include "game/multiplayer/platform/session/game_adapter.h"

namespace multiplayer::platform {

enum class HostLifecycle : uint8_t { IDLE, STARTING, READY, FAILED };
enum class HostMappingState : uint8_t { NOT_REQUESTED, DISABLED, PENDING, READY, FAILED };
enum class HostAccessKind : uint8_t { NONE, INVITE, ROOM_CODE };

struct DiscoverySnapshot {
  DiscoveryKind kind = DiscoveryKind::NONE;
  DiscoveryStatus status = DiscoveryStatus::IDLE;
  bool result_available = false;
};

struct HostSnapshot {
  HostLifecycle lifecycle = HostLifecycle::IDLE;
  HostMappingState mapping = HostMappingState::NOT_REQUESTED;
  HostAccessKind access = HostAccessKind::NONE;
  uint16_t port = 0;
  std::string access_text;
};

struct RuntimeSnapshot {
  SessionSnapshot session;
  DiscoverySnapshot discovery;
  HostSnapshot host;
  CommandResult connection_result;
  CommandResult session_result;
  CommandResult control_result;
  std::string compatibility_identity;
};

class RuntimeWorker;

class MultiplayerRuntime {
 public:
  MultiplayerRuntime() = default;
  ~MultiplayerRuntime();
  MultiplayerRuntime(const MultiplayerRuntime&) = delete;
  MultiplayerRuntime& operator=(const MultiplayerRuntime&) = delete;
  bool install(std::unique_ptr<GameAdapter> adapter,
               const std::optional<ProfileStorageConfig>& profile_config = std::nullopt);
  GameAdapter* adapter(std::string_view game_id) const;
  bool enqueue(std::unique_ptr<RuntimeCommand> command);

  template <typename Command, typename... Args>
    requires std::derived_from<Command, RuntimeCommand>
  bool enqueue(Args&&... args) {
    return enqueue(std::make_unique<Command>(std::forward<Args>(args)...));
  }

  RuntimeSnapshot snapshot() const;
  void shutdown();
  bool active() const;
  bool load_profile(StoredPlayerProfile& profile) const;
  bool save_profile(const StoredPlayerProfile& profile);
  fs::path game_preferences_path() const;

 private:
  friend class RuntimeWorker;

  struct QueuedCommand {
    uint32_t revision = 0;
    std::unique_ptr<RuntimeCommand> command;
  };

  std::deque<QueuedCommand> take_commands(size_t limit);
  void complete_command(CommandDomain domain,
                        uint32_t revision,
                        uint8_t action,
                        CommandError error);
  void publish_worker_snapshot(SessionSnapshot session,
                               DiscoverySnapshot discovery,
                               HostSnapshot host,
                               std::string compatibility_identity);
  CommandResult& result_for(CommandDomain domain);
  void run();
  void clear_locked();

  mutable std::mutex mutex_;
  std::condition_variable wake_cv_;
  std::unique_ptr<GameAdapter> adapter_;
  std::optional<ProfileLease> profile_lease_;
  StoredPlayerProfile stored_profile_;
  std::deque<QueuedCommand> commands_;
  RuntimeSnapshot snapshot_;
  uint32_t next_revision_ = 0;
  bool accepting_commands_ = false;
  std::thread worker_;
};

MultiplayerRuntime& multiplayer_runtime();
bool install_adapter(std::unique_ptr<GameAdapter> adapter, ProfileStorageConfig profile_config);
void shutdown();

}  // namespace multiplayer::platform
