#include "common/log/log.h"

#include "game/external/steam/steam_client.h"
#include "game/multiplayer/platform/discovery/invite.h"
#include "game/multiplayer/platform/runtime/multiplayer_runtime_worker.h"
#include "game/multiplayer/platform/session/connection_input.h"
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
  steam_lobby_.leave();
}

void RuntimeWorker::reset_host_state() {
  host_ = {};
  lobby_details_ = {};
  privacy_ = LobbyPrivacy::PUBLIC;
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
  const bool direct = endpoint.steam_host == 0;
  if ((direct && (endpoint.address.empty() || endpoint.port == 0)) ||
      endpoint.room_code.size() != 6) {
    return CommandError::INVALID_REQUEST;
  }
  stop_connection_services();
  reset_host_state();
  client_.endpoint = std::move(endpoint);
  if (reset_retry)
    reset_reconnect(reconnect_);
  ControllerClientConfig config = {.endpoint = client_.endpoint->address,
                                   .port = client_.endpoint->port,
                                   .room_code = client_.endpoint->room_code,
                                   .local_profile = client_.profile,
                                   .steam_host = client_.endpoint->steam_host};
  StoredPlayerProfile stored;
  if (runtime_.load_profile(stored))
    config.local_profile.player_id = stored.player_id;
  if (direct)
    lg::debug("[Multiplayer] Connecting to {}:{}.", config.endpoint, config.port);
  else
    lg::debug("[Multiplayer] Connecting to Steam host {}.", config.steam_host);
  return controller_.connect(config) ? CommandError::NONE : CommandError::START_FAILED;
}

CommandError RuntimeWorker::host(HostSessionRequest request) {
  reset_for_new_connection();
  internet_host_ = request.internet;
  automatic_port_mapping_ = request.automatic_port_mapping;
  privacy_ = request.privacy;
  host_ = {.lifecycle = HostLifecycle::STARTING,
           .mapping = request.internet
                          ? (request.automatic_port_mapping ? HostMappingState::PENDING
                                                            : HostMappingState::DISABLED)
                          : HostMappingState::NOT_REQUESTED};
  auto& config = request.config;
  StoredPlayerProfile stored;
  if (runtime_.load_profile(stored))
    config.local_profile.player_id = stored.player_id;
  config.steam = request.internet && steam::networking().has_value();
  lg::debug("[Multiplayer] Starting host on UDP port {}.", config.port);
  bool started = controller_.host(config);
  if (!started && config.steam) {
    lg::warn("[Multiplayer] Steam hosting failed; hosting directly instead.");
    config.steam = false;
    started = controller_.host(config);
  }
  if (!started) {
    host_.lifecycle = HostLifecycle::FAILED;
    return CommandError::START_FAILED;
  }
  const auto host_port = controller_.local_port();
  if (request.discovery_port != 0) {
    const auto& descriptor = adapter_.descriptor();
    discovery_responder_.set_listed(privacy_ == LobbyPrivacy::PUBLIC);
    if (!discovery_responder_.start(request.discovery_port,
                                    {.game_port = host_port,
                                     .current_players = 1,
                                     .player_limit = config.player_limit,
                                     .game_id = descriptor.game_id,
                                     .compatibility_identity = compatibility_identity_,
                                     .room_code = config.room_code,
                                     .host_name = config.local_profile.display_name})) {
      lg::error("[Multiplayer] Host discovery responder failed on UDP port {}.",
                request.discovery_port);
    }
  }
  if (request.internet && request.automatic_port_mapping)
    port_mapping_.start(host_port, host_port);
  if (config.steam && !steam_lobby_.create({.game_id = adapter_.descriptor().game_id,
                                            .compatibility_identity = compatibility_identity_,
                                            .room_code = config.room_code,
                                            .host_name = config.local_profile.display_name,
                                            .players = 1,
                                            .player_limit = config.player_limit},
                                           privacy_)) {
    lg::warn("[Multiplayer] Steam lobby creation failed; players can still join directly.");
  }
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

CommandError RuntimeWorker::search_lobbies() {
  const auto& game_id = adapter_.descriptor().game_id;
  discovery_result_.reset();
  discovery_kind_ =
      discovery_scanner_.start({.discovery_port = kMultiplayerDiscoveryPort, .game_id = game_id})
          ? DiscoveryKind::LAN
          : DiscoveryKind::NONE;
  const bool steam = steam_lobby_.search(game_id);
  return discovery_kind_ == DiscoveryKind::LAN || steam ? CommandError::NONE
                                                        : CommandError::UNAVAILABLE;
}

CommandError RuntimeWorker::join_lobby(JoinLobbyRequest request) {
  reset_for_new_connection();
  controller_.disconnect();
  client_.profile = std::move(request.profile);
  if (!steam_lobby_.join(request.lobby_id, adapter_.descriptor().game_id))
    return CommandError::UNAVAILABLE;
  discovery_kind_ = DiscoveryKind::STEAM;
  return CommandError::NONE;
}

CommandError RuntimeWorker::invite_friends() {
  return steam_lobby_.invite_friends() ? CommandError::NONE : CommandError::UNAVAILABLE;
}

CommandError RuntimeWorker::set_lobby_privacy(const LobbyPrivacy privacy) {
  privacy_ = privacy;
  steam_lobby_.set_privacy(privacy);
  discovery_responder_.set_listed(privacy == LobbyPrivacy::PUBLIC);
  return CommandError::NONE;
}

CommandError RuntimeWorker::set_lobby_details(std::string details) {
  lobby_details_ = std::move(details);
  return CommandError::NONE;
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
  if (!controller_.set_local_profile(profile))
    return CommandError::INVALID_STATE;
  client_.profile = profile;
  return CommandError::NONE;
}

CommandError RuntimeWorker::set_character(const PlayerCharacter character) {
  if (!controller_.set_character(character))
    return CommandError::INVALID_STATE;
  client_.profile.character = character;
  return CommandError::NONE;
}

CommandError RuntimeWorker::set_ready(const bool ready) {
  if (!controller_.set_ready(ready))
    return CommandError::INVALID_STATE;
  client_.profile.ready = ready;
  if (ready)
    persist_profile();
  return CommandError::NONE;
}

CommandError RuntimeWorker::send_chat(std::string text) {
  return controller_.send_chat(std::move(text)) ? CommandError::NONE
                                                : CommandError::INVALID_REQUEST;
}

CommandError RuntimeWorker::kick_player(const PlayerSlot player_slot) {
  return controller_.kick(player_slot) ? CommandError::NONE : CommandError::INVALID_STATE;
}

CommandError RuntimeWorker::start_countdown(const uint32_t seconds) {
  if (!controller_.start_countdown(seconds))
    return CommandError::INVALID_STATE;
  persist_profile();
  return CommandError::NONE;
}

CommandError RuntimeWorker::cancel_countdown() {
  return controller_.cancel_countdown() ? CommandError::NONE : CommandError::INVALID_STATE;
}

CommandError RuntimeWorker::start_game() {
  if (!controller_.start_game())
    return CommandError::INVALID_STATE;
  persist_profile();
  return CommandError::NONE;
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
