#include "game/multiplayer/platform/runtime/multiplayer_runtime.h"

#include <ranges>

#include "game/multiplayer/platform/runtime/multiplayer_runtime_worker.h"
#include "game/multiplayer/platform/session/packet_registry.h"

namespace multiplayer::platform {

namespace {
constexpr size_t kMailboxCapacity = 256;
}

MultiplayerRuntime::~MultiplayerRuntime() {
  shutdown();
}

bool MultiplayerRuntime::install(std::unique_ptr<GameAdapter> adapter,
                                 const std::optional<ProfileStorageConfig>& profile_config) {
  if (!adapter)
    return false;
  const auto& descriptor = adapter->descriptor();
  if (descriptor.game_id.empty() || !valid_player_limit(descriptor.maximum_players) ||
      !validate_message_policies(adapter->packets().policies(), descriptor.maximum_payload_bytes) ||
      descriptor.supported_characters.empty() ||
      std::ranges::any_of(
          descriptor.supported_characters,
          [](const PlayerCharacter character) { return character == PlayerCharacter::UNKNOWN; }) ||
      (profile_config && profile_config->game_id != descriptor.game_id)) {
    return false;
  }
  auto lease =
      profile_config ? ProfileLease::acquire(*profile_config) : std::optional<ProfileLease>{};
  if (profile_config && !lease)
    return false;
  StoredPlayerProfile stored_profile;
  if (lease && !load_player_profile(*lease, stored_profile))
    return false;
  std::lock_guard lock(mutex_);
  if (adapter_ || worker_.joinable())
    return false;
  snapshot_ = {};
  snapshot_.compatibility_identity = descriptor.compatibility_identity;
  adapter_ = std::move(adapter);
  profile_lease_ = std::move(lease);
  stored_profile_ = std::move(stored_profile);
  accepting_commands_ = true;
  worker_ = std::thread([this] { run(); });
  return true;
}

GameAdapter* MultiplayerRuntime::adapter(const std::string_view game_id) const {
  std::lock_guard lock(mutex_);
  return adapter_ && adapter_->descriptor().game_id == game_id ? adapter_.get() : nullptr;
}

CommandResult& MultiplayerRuntime::result_for(const CommandDomain domain) {
  if (domain == CommandDomain::CONNECTION)
    return snapshot_.connection_result;
  if (domain == CommandDomain::SESSION)
    return snapshot_.session_result;
  return snapshot_.control_result;
}

bool MultiplayerRuntime::enqueue(std::unique_ptr<RuntimeCommand> command) {
  if (!command)
    return false;
  std::lock_guard lock(mutex_);
  const auto revision = ++next_revision_;
  auto& result = result_for(command->domain());
  result = {.revision = revision, .action = command->action()};
  if (!accepting_commands_ || !adapter_) {
    result.outcome = CommandOutcome::REJECTED;
    result.error = CommandError::RUNTIME_INACTIVE;
    return false;
  }
  if (commands_.size() >= kMailboxCapacity) {
    result.outcome = CommandOutcome::REJECTED;
    result.error = CommandError::QUEUE_FULL;
    return false;
  }
  result.outcome = CommandOutcome::QUEUED;
  commands_.push_back({revision, std::move(command)});
  wake_cv_.notify_one();
  return true;
}

std::deque<MultiplayerRuntime::QueuedCommand> MultiplayerRuntime::take_commands(
    const size_t limit) {
  std::deque<QueuedCommand> result;
  std::lock_guard lock(mutex_);
  while (!commands_.empty() && result.size() < limit) {
    result.push_back(std::move(commands_.front()));
    commands_.pop_front();
  }
  return result;
}

void MultiplayerRuntime::complete_command(const CommandDomain domain,
                                          const uint32_t revision,
                                          const uint8_t action,
                                          const CommandError error) {
  std::lock_guard lock(mutex_);
  auto& result = result_for(domain);
  if (result.revision != revision || result.outcome != CommandOutcome::QUEUED)
    return;
  result.action = action;
  result.outcome = error == CommandError::NONE ? CommandOutcome::APPLIED : CommandOutcome::REJECTED;
  result.error = error;
}

void MultiplayerRuntime::publish_worker_snapshot(SessionSnapshot session,
                                                 const DiscoverySnapshot discovery,
                                                 HostSnapshot host,
                                                 std::string compatibility_identity) {
  std::lock_guard lock(mutex_);
  snapshot_.session = std::move(session);
  snapshot_.discovery = discovery;
  snapshot_.host = std::move(host);
  snapshot_.compatibility_identity = std::move(compatibility_identity);
}

RuntimeSnapshot MultiplayerRuntime::snapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

void MultiplayerRuntime::shutdown() {
  std::thread worker;
  {
    std::lock_guard lock(mutex_);
    accepting_commands_ = false;
    if (!worker_.joinable()) {
      clear_locked();
      return;
    }
    worker = std::move(worker_);
  }
  wake_cv_.notify_all();
  worker.join();
  std::lock_guard lock(mutex_);
  clear_locked();
}

void MultiplayerRuntime::clear_locked() {
  adapter_.reset();
  profile_lease_.reset();
  stored_profile_ = {};
  commands_.clear();
  snapshot_ = {};
  next_revision_ = 0;
  accepting_commands_ = false;
}

bool MultiplayerRuntime::active() const {
  std::lock_guard lock(mutex_);
  return accepting_commands_ && adapter_ != nullptr;
}

bool MultiplayerRuntime::load_profile(StoredPlayerProfile& profile) const {
  std::lock_guard lock(mutex_);
  if (!profile_lease_)
    return false;
  profile = stored_profile_;
  return true;
}

bool MultiplayerRuntime::save_profile(const StoredPlayerProfile& profile) {
  std::lock_guard lock(mutex_);
  if (!profile_lease_ || !save_player_profile(*profile_lease_, profile))
    return false;
  stored_profile_ = profile;
  return true;
}

fs::path MultiplayerRuntime::game_preferences_path() const {
  std::lock_guard lock(mutex_);
  return profile_lease_ ? profile_lease_->game_preferences_path() : fs::path{};
}

void MultiplayerRuntime::run() {
  GameAdapter* current = nullptr;
  {
    std::lock_guard lock(mutex_);
    current = adapter_.get();
  }
  if (current)
    RuntimeWorker(*this, *current).run();
}

MultiplayerRuntime& multiplayer_runtime() {
  static MultiplayerRuntime runtime;
  return runtime;
}

bool install_adapter(std::unique_ptr<GameAdapter> adapter, ProfileStorageConfig profile_config) {
  return multiplayer_runtime().install(std::move(adapter), std::move(profile_config));
}

void shutdown() {
  multiplayer_runtime().shutdown();
}

}  // namespace multiplayer::platform
