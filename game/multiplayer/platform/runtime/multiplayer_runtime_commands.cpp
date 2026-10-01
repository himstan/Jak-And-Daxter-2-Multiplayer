#include "common/log/log.h"

#include "game/multiplayer/platform/discovery/invite.h"
#include "game/multiplayer/platform/runtime/multiplayer_runtime_worker.h"
#ifdef ENABLE_NETWORK_SIMULATION
#include "game/multiplayer/platform/transport/network_simulation.h"
#endif

namespace multiplayer::platform {

RuntimeWorker::RuntimeWorker(MultiplayerRuntime& runtime, GameAdapter& adapter)
    : runtime_(runtime),
      adapter_(adapter),
      controller_(adapter),
      compatibility_identity_(adapter.descriptor().compatibility_identity) {}

void RuntimeWorker::stop_connection_services() {
  stop_discovery();
  discovery_responder_.stop();
  port_mapping_.stop();
}

void RuntimeWorker::reset_host_state() {
  host_ = {};
  internet_host_ = false;
  automatic_port_mapping_ = false;
}

void RuntimeWorker::reset_for_new_connection() {
  stop_connection_services();
  reset_reconnect(reconnect_);
  client_.endpoint.reset();
  reset_host_state();
}

CommandError RuntimeWorker::connect_canonical(CanonicalEndpoint endpoint, const bool reset_retry) {
  if (endpoint.address.empty() || endpoint.port == 0 || endpoint.room_code.size() != 6)
    return CommandError::INVALID_REQUEST;
  stop_connection_services();
  reset_host_state();
  client_.endpoint = std::move(endpoint);
  if (reset_retry)
    reset_reconnect(reconnect_);
  ControllerClientConfig config = {.endpoint = client_.endpoint->address,
                                   .port = client_.endpoint->port,
                                   .room_code = client_.endpoint->room_code,
                                   .local_profile = client_.profile};
  lg::debug("[Multiplayer] Connecting to {}:{}.", config.endpoint, config.port);
  return controller_.connect(config) ? CommandError::NONE : CommandError::START_FAILED;
}

CommandError RuntimeWorker::host(HostSessionRequest request) {
  reset_for_new_connection();
  internet_host_ = request.internet;
  automatic_port_mapping_ = request.automatic_port_mapping;
  host_ = {.lifecycle = HostLifecycle::STARTING,
           .mapping = request.internet
                          ? (request.automatic_port_mapping ? HostMappingState::PENDING
                                                            : HostMappingState::DISABLED)
                          : HostMappingState::NOT_REQUESTED};
  lg::debug("[Multiplayer] Starting host on UDP port {}.", request.config.port);
  if (!controller_.host(request.config)) {
    host_.lifecycle = HostLifecycle::FAILED;
    return CommandError::START_FAILED;
  }
  const auto host_port = controller_.local_port();
  if (request.discovery_port != 0) {
    const auto& descriptor = adapter_.descriptor();
    if (!discovery_responder_.start(request.discovery_port,
                                    {.game_port = host_port,
                                     .current_players = 1,
                                     .player_limit = request.config.player_limit,
                                     .game_id = descriptor.game_id,
                                     .compatibility_identity = compatibility_identity_,
                                     .room_code = request.config.room_code})) {
      lg::error("[Multiplayer] Host discovery responder failed on UDP port {}.",
                request.discovery_port);
    }
  }
  if (request.internet && request.automatic_port_mapping)
    port_mapping_.start(host_port, host_port);
  return CommandError::NONE;
}

CommandError RuntimeWorker::connect(ControllerClientConfig config) {
  client_.profile = std::move(config.local_profile);
  return connect_canonical({.address = std::move(config.endpoint),
                            .port = config.port,
                            .room_code = std::move(config.room_code)});
}

CommandError RuntimeWorker::start_discovery(DiscoveryRequest request) {
  if (request.discovery_port == 0)
    return CommandError::INVALID_REQUEST;
  reset_for_new_connection();
  controller_.disconnect();
  client_.profile = std::move(request.profile);
  discovery_kind_ = request.directed_address.empty() ? DiscoveryKind::LAN : DiscoveryKind::DIRECTED;
  const auto& descriptor = adapter_.descriptor();
  if (!discovery_scanner_.start(
          {.discovery_port = request.discovery_port,
           .expected_game_port = request.expected_game_port,
           .game_id = descriptor.game_id,
           .compatibility_identity = compatibility_identity_,
           .directed_address = std::move(request.directed_address),
           .include_full_sessions = discovery_kind_ == DiscoveryKind::DIRECTED})) {
    discovery_kind_ = DiscoveryKind::NONE;
    return CommandError::START_FAILED;
  }
  return CommandError::NONE;
}

CommandError RuntimeWorker::stop_discovery() {
  discovery_scanner_.stop();
  discovery_result_.reset();
  discovery_kind_ = DiscoveryKind::NONE;
  return CommandError::NONE;
}

CommandError RuntimeWorker::connect_discovered() {
  if (!discovery_result_)
    return CommandError::UNAVAILABLE;
  std::string address;
  std::string room_code;
  uint16_t port = 0;
  if (!parse_invite(discovery_result_->invite, address, port, room_code))
    return CommandError::INVALID_REQUEST;
  return connect_canonical(
      {.address = std::move(address), .port = port, .room_code = std::move(room_code)});
}

CommandError RuntimeWorker::reconnect() {
  if (!client_.endpoint)
    return CommandError::UNAVAILABLE;
  controller_.disconnect();
  const auto result = connect_canonical(*client_.endpoint, false);
  if (result == CommandError::NONE)
    mark_reconnect_attempt_started(reconnect_);
  else
    mark_reconnect_attempt_failed(reconnect_, steady_time_ms());
  return result;
}

CommandError RuntimeWorker::disconnect(const int reason) {
  reset_for_new_connection();
  controller_.disconnect(reason);
  return CommandError::NONE;
}

CommandError RuntimeWorker::set_profile(PlayerProfile profile) {
  client_.profile = profile;
  return controller_.set_local_profile(std::move(profile)) ? CommandError::NONE
                                                           : CommandError::INVALID_STATE;
}

CommandError RuntimeWorker::set_character(const PlayerCharacter character) {
  client_.profile.character = character;
  return controller_.set_character(character) ? CommandError::NONE : CommandError::INVALID_STATE;
}

CommandError RuntimeWorker::set_ready(const bool ready) {
  client_.profile.ready = ready;
  return controller_.set_ready(ready) ? CommandError::NONE : CommandError::INVALID_STATE;
}

CommandError RuntimeWorker::start_countdown(const uint32_t seconds) {
  return controller_.start_countdown(seconds) ? CommandError::NONE : CommandError::INVALID_STATE;
}

CommandError RuntimeWorker::cancel_countdown() {
  return controller_.cancel_countdown() ? CommandError::NONE : CommandError::INVALID_STATE;
}

CommandError RuntimeWorker::start_game() {
  return controller_.start_game() ? CommandError::NONE : CommandError::INVALID_STATE;
}

CommandError RuntimeWorker::session_action(void (SessionController::*action)()) {
  if (controller_.snapshot().state.role == SessionRole::NONE)
    return CommandError::INVALID_STATE;
  (controller_.*action)();
  return CommandError::NONE;
}

CommandError RuntimeWorker::enter_lobby() {
  return session_action(&SessionController::enter_lobby);
}

CommandError RuntimeWorker::enter_game() {
  return session_action(&SessionController::enter_game);
}

CommandError RuntimeWorker::request_bootstrap() {
  return session_action(&SessionController::request_bootstrap);
}

CommandError RuntimeWorker::configure_compatibility_identity(std::string identity) {
  if (controller_.snapshot().state.status != SessionStatus::IDLE ||
      discovery_scanner_.status() != DiscoveryStatus::IDLE) {
    return CommandError::NOT_ALLOWED;
  }
  if (!adapter_.configure_compatibility_identity(std::move(identity)))
    return CommandError::INVALID_REQUEST;
  compatibility_identity_ = adapter_.descriptor().compatibility_identity;
  return CommandError::NONE;
}

#ifdef ENABLE_NETWORK_SIMULATION
CommandError RuntimeWorker::set_network_simulation(NetworkSimulationSettings settings) {
  apply_network_simulation(settings);
  return CommandError::NONE;
}
#endif

}  // namespace multiplayer::platform
