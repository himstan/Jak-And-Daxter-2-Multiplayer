#include "game/multiplayer/platform/runtime/multiplayer_runtime_worker.h"

#include <chrono>
#include <exception>

#include "common/log/log.h"

namespace multiplayer::platform {

namespace {
constexpr size_t kMaximumCommandsPerIteration = 32;
}

uint64_t RuntimeWorker::steady_time_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

void RuntimeWorker::process_commands() {
  RuntimeExecutionPorts ports{*this, *this, *this};
  for (auto& queued : runtime_.take_commands(kMaximumCommandsPerIteration)) {
    const auto domain = queued.command->domain();
    const auto action = queued.command->action();
    CommandError error = CommandError::NONE;
    try {
      error = queued.command->execute(ports);
    } catch (const std::exception& exception) {
      error = CommandError::INTERNAL_ERROR;
      lg::error("[Multiplayer] Runtime command failed: {}", exception.what());
    } catch (...) {
      error = CommandError::INTERNAL_ERROR;
      lg::error("[Multiplayer] Runtime command failed with an unknown exception.");
    }
    runtime_.complete_command(domain, queued.revision, action, error);
  }
}

void RuntimeWorker::pump_reconnect(const uint64_t now_ms) {
  auto status = controller_.snapshot().state.status;
  if (status == SessionStatus::RECONNECTING && client_.endpoint && !reconnect_.attempt_active &&
      reconnect_.next_attempt_time_ms == 0) {
    schedule_reconnect(reconnect_, now_ms);
  }
  if (client_.endpoint && reconnect_due(reconnect_, now_ms)) {
    const auto error = connect_canonical(*client_.endpoint, false);
    if (error == CommandError::NONE)
      mark_reconnect_attempt_started(reconnect_);
    else
      mark_reconnect_attempt_failed(reconnect_, now_ms);
    status = controller_.snapshot().state.status;
  }
  if (reconnect_.attempt_active && status == SessionStatus::LOBBY)
    mark_reconnect_authenticated(reconnect_);
  else if (reconnect_.attempt_active && status == SessionStatus::FAILED)
    mark_reconnect_attempt_failed(reconnect_, now_ms);
  else if (reconnect_.waiting_for_bootstrap && status == SessionStatus::IN_GAME)
    reset_reconnect(reconnect_);
}

void RuntimeWorker::persist_profile() {
  const auto& profile = controller_.local_profile();
  StoredPlayerProfile stored;
  if (runtime_.load_profile(stored) && stored.display_name == profile.display_name &&
      stored.preferred_character == profile.character)
    return;
  runtime_.save_profile(
      {.display_name = profile.display_name, .preferred_character = profile.character});
}

void RuntimeWorker::update_discovery() {
  if (auto result = discovery_scanner_.take_result())
    discovery_result_ = std::move(result);
}

void RuntimeWorker::update_host_advertisement(const SessionSnapshot& session) {
  if (session.state.role != SessionRole::HOST || !discovery_responder_.active())
    return;
  const auto& descriptor = adapter_.descriptor();
  discovery_responder_.update({.game_port = controller_.local_port(),
                               .current_players = static_cast<uint8_t>(session.players.size()),
                               .player_limit = session.state.player_limit,
                               .game_id = descriptor.game_id,
                               .compatibility_identity = compatibility_identity_,
                               .room_code = controller_.room_code()});
}

void RuntimeWorker::publish(const SessionSnapshot& session) {
  DiscoverySnapshot discovery = {.kind = discovery_kind_,
                                 .status = discovery_scanner_.status(),
                                 .result_available = discovery_result_.has_value()};
  if (discovery.result_available)
    discovery.status = DiscoveryStatus::FOUND;

  auto host = host_;
  if (session.state.role == SessionRole::HOST) {
    host.port = controller_.local_port();
    host.lifecycle = HostLifecycle::READY;
    host.access = HostAccessKind::ROOM_CODE;
    host.access_text = controller_.room_code();
    if (!internet_host_)
      host.mapping = HostMappingState::NOT_REQUESTED;
    else if (!automatic_port_mapping_)
      host.mapping = HostMappingState::DISABLED;
    else {
      const auto mapping = port_mapping_.snapshot();
      if (mapping.state == MPPortMappingState::PENDING)
        host.mapping = HostMappingState::PENDING;
      else if (mapping.state == MPPortMappingState::READY) {
        host.mapping = HostMappingState::READY;
        host.access = HostAccessKind::INVITE;
        host.access_text = controller_.invite_for_address(mapping.external_address);
      } else if (mapping.state == MPPortMappingState::FAILED)
        host.mapping = HostMappingState::FAILED;
    }
  }
  host_ = host;
  runtime_.publish_worker_snapshot(session, discovery, std::move(host), compatibility_identity_);
}

void RuntimeWorker::run(const std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    process_commands();
    const auto now_ms = steady_time_ms();
    controller_.pump(now_ms);
    pump_reconnect(now_ms);
    adapter_.tick(now_ms);
    update_discovery();
    const auto session = controller_.snapshot();
    update_host_advertisement(session);
    publish(session);
    std::unique_lock lock(runtime_.mutex_);
    runtime_.wake_cv_.wait_for(lock, stop_token, std::chrono::milliseconds(2),
                               [] { return false; });
  }
  stop_connection_services();
  controller_.disconnect();
  adapter_.stop();
}

}  // namespace multiplayer::platform
