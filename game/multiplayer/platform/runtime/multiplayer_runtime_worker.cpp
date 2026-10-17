#include "game/multiplayer/platform/runtime/multiplayer_runtime_worker.h"

#include <chrono>
#include <exception>

#include "common/log/log.h"

#include "game/external/steam/steam_client.h"

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
  if (discovery_kind_ != DiscoveryKind::DIRECTED || discovery_result_)
    return;
  if (auto results = discovery_scanner_.results(); !results.empty())
    discovery_result_ = std::move(results.front());
}

void RuntimeWorker::update_steam() {
  steam_lobby_.pump();
  if (steam_lobby_.take_create_failure())
    lg::warn("[Multiplayer] Steam lobby creation failed; players can still join directly.");
  if (auto target = steam_lobby_.take_join_target()) {
    if (connect_canonical({.room_code = std::move(target->room_code),
                           .steam_host = target->host_id}) != CommandError::NONE) {
      lg::error("[Multiplayer] Could not connect to Steam host {}.", target->host_id);
      discovery_kind_ = DiscoveryKind::STEAM;
      steam_lobby_.fail_join();
    }
  }
}

void RuntimeWorker::update_host_advertisement(const SessionSnapshot& session) {
  if (session.state.role != SessionRole::HOST)
    return;
  const auto players = static_cast<uint8_t>(session.players.size());
  const auto details =
      session.state.status == SessionStatus::LOBBY ? std::string() : lobby_details_;
  steam_lobby_.update_players(players);
  steam_lobby_.update_details(details);
  if (!discovery_responder_.active())
    return;
  const auto& descriptor = adapter_.descriptor();
  discovery_responder_.update({.game_port = controller_.local_port(),
                               .current_players = players,
                               .player_limit = session.state.player_limit,
                               .game_id = descriptor.game_id,
                               .compatibility_identity = compatibility_identity_,
                               .room_code = controller_.room_code(),
                               .host_name = controller_.local_profile().display_name,
                               .details = details});
}

void RuntimeWorker::publish(const SessionSnapshot& session) {
  DiscoverySnapshot discovery = {.kind = discovery_kind_,
                                 .status = discovery_kind_ == DiscoveryKind::STEAM
                                               ? steam_lobby_.join_status()
                                               : discovery_scanner_.status(),
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
        host.access_text =
            controller_.invite_for_address(mapping.external_address, mapping.external_port);
      } else if (mapping.state == MPPortMappingState::FAILED)
        host.mapping = HostMappingState::FAILED;
    }
    if (steam_lobby_.creating())
      host.lifecycle = HostLifecycle::STARTING;
    else if (steam_lobby_.hosting())
      host.access = HostAccessKind::STEAM;
  }
  host_ = host;
  const bool browsing_lan = discovery_kind_ == DiscoveryKind::LAN;
  const auto lan_games =
      browsing_lan ? discovery_scanner_.results() : std::vector<DiscoveryResult>{};
  auto games = merge_game_listings(lan_games, steam_lobby_.listings(), compatibility_identity_);
  const auto status =
      browser_status(browsing_lan ? discovery_scanner_.status() : DiscoveryStatus::IDLE,
                     steam_lobby_.search_status(), !games.empty());
  runtime_.publish_worker_snapshot(
      session, discovery, std::move(host),
      {.available = steam::networking().has_value(), .invite_lobby = steam_lobby_.invite()},
      {.status = status, .games = std::move(games)}, compatibility_identity_);
}

void RuntimeWorker::run() {
  std::unique_lock lock(runtime_.mutex_);
  while (runtime_.accepting_commands_) {
    lock.unlock();
    process_commands();
    const auto now_ms = steady_time_ms();
    controller_.pump(now_ms);
    pump_reconnect(now_ms);
    adapter_.tick(now_ms);
    update_discovery();
    update_steam();
    const auto session = controller_.snapshot();
    update_host_advertisement(session);
    publish(session);
    lock.lock();
    runtime_.wake_cv_.wait_for(lock, std::chrono::milliseconds(2),
                               [this] { return !runtime_.accepting_commands_; });
  }
  lock.unlock();
  stop_connection_services();
  controller_.disconnect();
  adapter_.stop();
}

}  // namespace multiplayer::platform
