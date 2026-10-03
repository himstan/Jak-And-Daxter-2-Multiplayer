#include "game/multiplayer/jak2/api/multiplayer_api.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <string>

#include "common/log/log.h"
#include "common/versions/versions.h"

#include "game/kernel/common/kmachine.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/multiplayer/jak2/api/preferences.h"
#include "game/multiplayer/jak2/application/jak2_adapter.h"
#include "game/multiplayer/jak2/bridge/goal_bridge.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/platform/core/compatibility_identity.h"
#include "game/multiplayer/platform/discovery/invite.h"
#include "game/multiplayer/platform/runtime/multiplayer_runtime.h"
#include "game/multiplayer/platform/session/connection_draft.h"

#include "third-party/SDL/include/SDL3/SDL.h"

namespace {

auto& runtime() {
  return multiplayer::platform::multiplayer_runtime();
}

auto& connection_draft() {
  static multiplayer::platform::ConnectionDraft draft;
  return draft;
}

multiplayer::platform::PlayerProfile local_profile() {
  const auto& preferences = multiplayer_preferences();
  multiplayer::platform::StoredPlayerProfile identity = {.display_name = preferences.player_name};
  runtime().load_profile(identity);
  multiplayer::platform::PlayerProfile profile;
  profile.display_name = std::move(identity.display_name);
  profile.character = identity.preferred_character;
  const auto* begin = reinterpret_cast<const uint8_t*>(&preferences.player_appearance);
  profile.game_extension.assign(begin, begin + sizeof(preferences.player_appearance));
  return profile;
}

multiplayer::platform::ControllerHostConfig host_config(const uint32_t player_limit) {
  multiplayer::platform::ControllerHostConfig config;
  config.port = get_resolved_host_port();
  config.player_limit = static_cast<uint8_t>(player_limit);
  config.room_code = get_resolved_host_room_code();
  config.local_profile = local_profile();
  return config;
}

uint64_t steady_time_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

bool enqueue_invite(const std::string& invite, multiplayer::platform::PlayerProfile profile) {
  multiplayer::platform::ControllerClientConfig request;
  if (!multiplayer::platform::build_connection_request(invite, std::move(profile), request))
    return false;
  return runtime().enqueue<multiplayer::platform::ConnectSessionCommand>(std::move(request));
}

void enqueue_host(const uint32_t player_limit, const bool internet) {
  runtime().enqueue<multiplayer::platform::HostSessionCommand>(
      multiplayer::platform::HostSessionRequest{
          .config = host_config(player_limit),
          .discovery_port = multiplayer::platform::kMultiplayerDiscoveryPort,
          .internet = internet,
          .automatic_port_mapping = internet && multiplayer_preferences().automatic_port_mapping});
}

const multiplayer::platform::CommandResult& command_result(
    const multiplayer::platform::RuntimeSnapshot& snapshot,
    const int domain) {
  if (domain == 0)
    return snapshot.connection_result;
  if (domain == 1)
    return snapshot.session_result;
  return snapshot.control_result;
}

}  // namespace

static u8 pc_multi_get_local_player_id() {
  return runtime().snapshot().session.state.local_player_id;
}

static u8 pc_multi_get_host_player_id() {
  return runtime().snapshot().session.state.host_player_id;
}

static int64_t pc_multi_get_player_ping(const u32 player_id) {
  if (player_id >= kMPMaxPlayers)
    return -1;
  const auto snapshot = runtime().snapshot();
  return snapshot.session.player_ping_ms(static_cast<multiplayer::platform::PlayerId>(player_id))
      .value_or(-1);
}

static u32 pc_multi_get_local_player_character() {
  for (const auto snapshot = runtime().snapshot(); const auto& profile : snapshot.session.players) {
    if (profile.player_id == snapshot.session.state.local_player_id)
      return static_cast<u32>(profile.character);
  }
  return static_cast<u32>(PlayerCharacter::UNKNOWN);
}

static int pc_multi_set_local_version(const u32 version_ptr) {
  std::string configured_version;
  if (!multiplayer::jak2::bridge::read_string(version_ptr, configured_version)) {
    return 0;
  }
  size_t length = 0;
  while (length <= multiplayer::platform::kCompatibilityIdentityMaxLength &&
         length < configured_version.size()) {
    ++length;
  }
  std::string resolved_version;
  if (length > multiplayer::platform::kCompatibilityIdentityMaxLength ||
      !multiplayer::platform::resolve_compatibility_identity(
          std::string_view(configured_version.data(), length), build_commit(), resolved_version)) {
    return 0;
  }
  return runtime().enqueue<multiplayer::platform::SetCompatibilityIdentityCommand>(
             std::move(resolved_version))
             ? 1
             : 0;
}

static u64 pc_multi_get_local_version() {
  const auto identity = runtime().snapshot().compatibility_identity;
  return jak2::make_string_from_c(identity.c_str());
}

static u64 pc_multi_get_required_version() {
  const auto view = runtime().snapshot();
  if (view.session.rejection != multiplayer::platform::RejectionReason::VERSION_MISMATCH) {
    return jak2::make_string_from_c("");
  }
  return jak2::make_string_from_c(view.session.required_identity.c_str());
}

static int pc_multi_exchange_state(const u32 state_ptr) {
  try {
    auto& service = multiplayer::jak2::application::jak2_adapter();
    return multiplayer::jak2::bridge::exchange_state(state_ptr, service.mailbox()) ? 1 : 0;
  } catch (...) {
    lg::error("[Multiplayer] Exception in pc_multi_exchange_state");
    return 0;
  }
}

void pc_multi_disconnect() {
  runtime().enqueue<multiplayer::platform::DisconnectSessionCommand>();
}

static void pc_multi_setup_host(const u32 player_limit) {
  enqueue_host(player_limit, false);
}

static void pc_multi_setup_internet_host(const u32 player_limit) {
  enqueue_host(player_limit, true);
}

static void pc_multi_setup_client(const u32 ip_ptr, const u32 port) {
  std::string invite;
  if (!multiplayer::jak2::bridge::read_string(ip_ptr, invite) || invite.empty()) {
    lg::warn("[Multiplayer] Ignoring setup-client with empty invite string.");
    return;
  }
  if (port != 0 && port <= (std::numeric_limits<uint16_t>::max)()) {
    runtime().enqueue<multiplayer::platform::StartDiscoveryCommand>(
        multiplayer::platform::DiscoveryRequest{
            .discovery_port = multiplayer::platform::kMultiplayerDiscoveryPort,
            .expected_game_port = static_cast<uint16_t>(port),
            .directed_address = std::move(invite),
            .profile = local_profile()});
  } else {
    enqueue_invite(std::move(invite), local_profile());
  }
}

static int pc_multi_get_session_status() {
  return static_cast<int>(runtime().snapshot().session.state.status);
}

static int pc_multi_get_session_rejection() {
  return static_cast<int>(runtime().snapshot().session.rejection);
}

static int pc_multi_get_discovery_kind() {
  return static_cast<int>(runtime().snapshot().discovery.kind);
}

static int pc_multi_get_discovery_status() {
  return static_cast<int>(runtime().snapshot().discovery.status);
}

static int pc_multi_discovery_result_available() {
  return runtime().snapshot().discovery.result_available ? 1 : 0;
}

static int pc_multi_get_command_outcome(const int domain) {
  const auto view = runtime().snapshot();
  return static_cast<int>(command_result(view, domain).outcome);
}

static int pc_multi_get_command_error(const int domain) {
  const auto view = runtime().snapshot();
  return static_cast<int>(command_result(view, domain).error);
}

static void pc_multi_session_enter_game() {
  runtime().enqueue<multiplayer::platform::EnterGameCommand>();
}

static void pc_multi_session_start_game() {
  runtime().enqueue<multiplayer::platform::StartGameCommand>();
}

static void pc_multi_session_enter_lobby() {
  runtime().enqueue<multiplayer::platform::EnterLobbyCommand>();
}

static void pc_multi_request_bootstrap() {
  runtime().enqueue<multiplayer::platform::RequestBootstrapCommand>();
}

static void pc_multi_stop_search() {
  runtime().enqueue<multiplayer::platform::StopDiscoveryCommand>();
}

static void pc_multi_start_search() {
  runtime().enqueue<multiplayer::platform::StartDiscoveryCommand>(
      multiplayer::platform::DiscoveryRequest{
          .discovery_port = multiplayer::platform::kMultiplayerDiscoveryPort,
          .profile = local_profile()});
}

static u64 pc_multi_get_command_line_arg(const u32 str_ptr) {
  std::string arg_name;
  if (!multiplayer::jak2::bridge::read_string(str_ptr, arg_name)) {
    return s7.offset;
  }
  for (int i = 1; i < g_argc; i++) {
    if (g_argv[i] && arg_name == g_argv[i]) {
      return jak2::make_string_from_c(i + 1 < g_argc ? g_argv[i + 1] : "");
    }
  }
  return s7.offset;
}

static int pc_multi_reconnect() {
  return runtime().enqueue<multiplayer::platform::ReconnectCommand>() ? 1 : 0;
}

static void pc_multi_clear_direct_connect() {
  connection_draft().clear();
}

static void pc_multi_reset_direct_connect() {
  connection_draft().reset(multiplayer_preferences().network_port);
}

static u64 pc_multi_get_direct_field(const int field) {
  const std::string display = connection_draft().field(field);
  return jak2::make_string_from_c(display.c_str());
}

static int pc_multi_set_direct_field(const int field, const u32 value_ptr) {
  std::string value;
  return multiplayer::jak2::bridge::read_string(value_ptr, value) &&
                 connection_draft().set_field(field, value)
             ? 1
             : 0;
}

static int pc_multi_direct_connect_ready() {
  return connection_draft().ready() ? 1 : 0;
}

static int pc_multi_connect_direct() {
  multiplayer::platform::DraftConnectionRequest request;
  if (!connection_draft().build_direct(local_profile(),
                                       multiplayer::platform::kMultiplayerDiscoveryPort, request))
    return 0;
  const bool accepted = request.connection
                            ? runtime().enqueue<multiplayer::platform::ConnectSessionCommand>(
                                  std::move(*request.connection))
                            : runtime().enqueue<multiplayer::platform::StartDiscoveryCommand>(
                                  std::move(*request.discovery));
  if (accepted && request.connection)
    connection_draft().clear();
  return accepted ? 1 : 0;
}

static u64 pc_multi_get_preference_field(const int field) {
  const std::string display = get_multiplayer_preference_display(field);
  return jak2::make_string_from_c(display.c_str());
}

static u64 pc_multi_get_player_name() {
  return jak2::make_string_from_c(multiplayer_preferences().player_name.c_str());
}

static int pc_multi_set_preference_field(const int field, const u32 value_ptr) {
  std::string value;
  return multiplayer::jak2::bridge::read_string(value_ptr, value) &&
                 set_multiplayer_preference(field, value)
             ? 1
             : 0;
}

static int pc_multi_get_automatic_port_mapping() {
  return multiplayer_preferences().automatic_port_mapping ? 1 : 0;
}

static void pc_multi_set_automatic_port_mapping(const int enabled) {
  set_automatic_port_mapping(enabled != 0);
}

static void pc_multi_reset_preferences() {
  reset_multiplayer_preferences();
}

static u32 pc_multi_get_preference_player_limit() {
  return get_session_player_limit_preference();
}

static void pc_multi_set_preference_player_limit(const u32 limit) {
  set_session_player_limit_preference(limit);
}

static u32 pc_multi_get_preference_player_character() {
  return get_player_character_preference();
}

static void pc_multi_set_preference_player_character(const u32 character) {
  set_player_character_preference(character);
}

static int pc_multi_is_lobby_host() {
  return runtime().snapshot().session.state.role == multiplayer::platform::SessionRole::HOST ? 1
                                                                                             : 0;
}

static u32 pc_multi_get_session_player_limit() {
  return runtime().snapshot().session.state.player_limit;
}

static int pc_multi_lobby_start_game() {
  return runtime().enqueue<multiplayer::platform::StartGameCommand>() ? 1 : 0;
}

static int pc_multi_lobby_set_character(u32 character) {
  if (const auto char_enum = static_cast<PlayerCharacter>(character);
      !is_player_character_valid(char_enum)) {
    return 0;
  }
  return runtime().enqueue<multiplayer::platform::SetCharacterCommand>(
             static_cast<PlayerCharacter>(character))
             ? 1
             : 0;
}

static int pc_multi_lobby_set_ready(const u32 ready) {
  return runtime().enqueue<multiplayer::platform::SetReadyCommand>(ready != 0) ? 1 : 0;
}

static int pc_multi_lobby_start_countdown(const u32 seconds) {
  return runtime().enqueue<multiplayer::platform::StartCountdownCommand>(seconds == 0 ? 3 : seconds)
             ? 1
             : 0;
}

static int pc_multi_lobby_cancel_countdown() {
  return runtime().enqueue<multiplayer::platform::CancelCountdownCommand>() ? 1 : 0;
}

static int64_t pc_multi_lobby_get_countdown_remaining_ms() {
  const auto view = runtime().snapshot().session;
  if (!view.countdown_active) {
    return -1;
  }
  const uint64_t now = steady_time_ms();
  if (now >= view.countdown_target_ms) {
    return 0;
  }
  return static_cast<int64_t>(view.countdown_target_ms - now);
}

static int pc_multi_lobby_is_countdown() {
  return runtime().snapshot().session.countdown_active ? 1 : 0;
}

static u32 pc_multi_get_player_color() {
  return multiplayer_preferences()
      .player_appearance.colors[player_appearance_group_index(MPPlayerAppearanceGroup::PRIMARY)];
}

static int pc_multi_get_player_appearance(const u32 appearance_ptr) {
  return multiplayer::jak2::bridge::write_appearance(appearance_ptr,
                                                     multiplayer_preferences().player_appearance)
             ? 1
             : 0;
}

static int pc_multi_lobby_set_appearance(const u32 appearance_ptr) {
  MPPlayerAppearance appearance = {};
  if (!multiplayer::jak2::bridge::read_appearance(appearance_ptr, appearance)) {
    return 0;
  }
  if (!set_player_appearance(appearance)) {
    return 0;
  }
  auto profile = local_profile();
  for (const auto snapshot = runtime().snapshot(); const auto& player : snapshot.session.players) {
    if (player.player_id == snapshot.session.state.local_player_id) {
      profile.character = player.character;
      profile.ready = player.ready;
      break;
    }
  }
  return runtime().enqueue<multiplayer::platform::SetProfileCommand>(std::move(profile)) ? 1 : 0;
}

static int pc_multi_get_host_lifecycle() {
  return static_cast<int>(runtime().snapshot().host.lifecycle);
}

static int pc_multi_get_host_mapping_state() {
  return static_cast<int>(runtime().snapshot().host.mapping);
}

static int pc_multi_get_host_port() {
  const auto port = runtime().snapshot().host.port;
  return port != 0 ? port : get_resolved_host_port();
}

static void pc_multi_connect_found_host() {
  runtime().enqueue<multiplayer::platform::ConnectDiscoveredCommand>();
}

static int pc_multi_get_host_access_kind() {
  return static_cast<int>(runtime().snapshot().host.access);
}

static int pc_multi_copy_host_access() {
  const std::string payload = runtime().snapshot().host.access_text;
  if (payload.empty()) {
    return 0;
  }
  const bool copied = SDL_SetClipboardText(payload.c_str());
  return copied ? 1 : 0;
}

static void pc_multi_clear_staged_invite() {
  connection_draft().clear_staged();
}

static int pc_multi_stage_clipboard_invite() {
  char* clipboard_text = SDL_GetClipboardText();
  if (!clipboard_text) {
    return 0;
  }

  size_t length = 0;
  while (length <= multiplayer::platform::kMaximumInviteLength && clipboard_text[length] != '\0') {
    ++length;
  }
  const bool valid = length <= multiplayer::platform::kMaximumInviteLength;
  std::string candidate = valid ? std::string(clipboard_text, length) : std::string();
  SDL_free(clipboard_text);

  return connection_draft().stage(std::move(candidate)) ? 1 : 0;
}

static int pc_multi_get_staged_invite_status() {
  return connection_draft().staged() ? 1 : 0;
}

static void pc_multi_connect_staged_invite() {
  multiplayer::platform::ControllerClientConfig request;
  if (connection_draft().build_staged(local_profile(), request))
    runtime().enqueue<multiplayer::platform::ConnectSessionCommand>(std::move(request));
}

void init_jak2_bridge() {
  load_multiplayer_preferences();
  const auto register_symbol = [](const char* name, auto function) {
    jak2::make_function_symbol_from_c(name, reinterpret_cast<void*>(function));
  };
  register_symbol("pc-multi-set-local-version", &pc_multi_set_local_version);
  register_symbol("pc-multi-get-local-version", &pc_multi_get_local_version);
  register_symbol("pc-multi-get-required-version", &pc_multi_get_required_version);
  register_symbol("pc-multi-setup-host", &pc_multi_setup_host);
  register_symbol("pc-multi-setup-internet-host", &pc_multi_setup_internet_host);
  register_symbol("pc-multi-setup-client", &pc_multi_setup_client);
  register_symbol("pc-multi-get-session-status", &pc_multi_get_session_status);
  register_symbol("pc-multi-get-session-rejection", &pc_multi_get_session_rejection);
  register_symbol("pc-multi-get-discovery-kind", &pc_multi_get_discovery_kind);
  register_symbol("pc-multi-get-discovery-status", &pc_multi_get_discovery_status);
  register_symbol("pc-multi-discovery-result-available", &pc_multi_discovery_result_available);
  register_symbol("pc-multi-get-command-outcome", &pc_multi_get_command_outcome);
  register_symbol("pc-multi-get-command-error", &pc_multi_get_command_error);
  register_symbol("pc-multi-session-enter-game", &pc_multi_session_enter_game);
  register_symbol("pc-multi-session-start-game", &pc_multi_session_start_game);
  register_symbol("pc-multi-session-enter-lobby", &pc_multi_session_enter_lobby);
  register_symbol("pc-multi-request-bootstrap", &pc_multi_request_bootstrap);
  register_symbol("pc-multi-stop-search", &pc_multi_stop_search);
  register_symbol("pc-multi-start-search", &pc_multi_start_search);
  register_symbol("pc-multi-connect-found-host", &pc_multi_connect_found_host);
  register_symbol("pc-multi-get-host-access-kind", &pc_multi_get_host_access_kind);
  register_symbol("pc-multi-copy-host-access", &pc_multi_copy_host_access);
  register_symbol("pc-multi-stage-clipboard-invite", &pc_multi_stage_clipboard_invite);
  register_symbol("pc-multi-get-staged-invite-status", &pc_multi_get_staged_invite_status);
  register_symbol("pc-multi-connect-staged-invite", &pc_multi_connect_staged_invite);
  register_symbol("pc-multi-clear-staged-invite", &pc_multi_clear_staged_invite);
  register_symbol("pc-multi-clear-direct-connect", &pc_multi_clear_direct_connect);
  register_symbol("pc-multi-reset-direct-connect", &pc_multi_reset_direct_connect);
  register_symbol("pc-multi-get-direct-field", &pc_multi_get_direct_field);
  register_symbol("pc-multi-set-direct-field", &pc_multi_set_direct_field);
  register_symbol("pc-multi-direct-connect-ready", &pc_multi_direct_connect_ready);
  register_symbol("pc-multi-connect-direct", &pc_multi_connect_direct);
  register_symbol("pc-multi-get-preference-field", &pc_multi_get_preference_field);
  register_symbol("pc-multi-get-player-name", &pc_multi_get_player_name);
  register_symbol("pc-multi-set-preference-field", &pc_multi_set_preference_field);
  register_symbol("pc-multi-get-automatic-port-mapping", &pc_multi_get_automatic_port_mapping);
  register_symbol("pc-multi-set-automatic-port-mapping", &pc_multi_set_automatic_port_mapping);
  register_symbol("pc-multi-reset-preferences", &pc_multi_reset_preferences);
  register_symbol("pc-multi-get-preference-player-limit", &pc_multi_get_preference_player_limit);
  register_symbol("pc-multi-set-preference-player-limit", &pc_multi_set_preference_player_limit);
  register_symbol("pc-multi-get-preference-player-character",
                  &pc_multi_get_preference_player_character);
  register_symbol("pc-multi-set-preference-player-character",
                  &pc_multi_set_preference_player_character);
  register_symbol("pc-multi-is-lobby-host", &pc_multi_is_lobby_host);
  register_symbol("pc-multi-get-session-player-limit", &pc_multi_get_session_player_limit);
  register_symbol("pc-multi-lobby-start-game", &pc_multi_lobby_start_game);
  register_symbol("pc-multi-lobby-set-character", &pc_multi_lobby_set_character);
  register_symbol("pc-multi-lobby-set-ready", &pc_multi_lobby_set_ready);
  register_symbol("pc-multi-lobby-start-countdown", &pc_multi_lobby_start_countdown);
  register_symbol("pc-multi-lobby-cancel-countdown", &pc_multi_lobby_cancel_countdown);
  register_symbol("pc-multi-lobby-get-countdown-remaining-ms",
                  &pc_multi_lobby_get_countdown_remaining_ms);
  register_symbol("pc-multi-lobby-is-countdown", &pc_multi_lobby_is_countdown);
  register_symbol("pc-multi-get-player-color", &pc_multi_get_player_color);
  register_symbol("pc-multi-get-player-appearance", &pc_multi_get_player_appearance);
  register_symbol("pc-multi-lobby-set-appearance", &pc_multi_lobby_set_appearance);
  register_symbol("pc-multi-get-host-lifecycle", &pc_multi_get_host_lifecycle);
  register_symbol("pc-multi-get-host-mapping-state", &pc_multi_get_host_mapping_state);
  register_symbol("pc-multi-get-host-port", &pc_multi_get_host_port);
  register_symbol("pc-multi-exchange-state", &pc_multi_exchange_state);
  register_symbol("pc-multi-get-local-player-id", &pc_multi_get_local_player_id);
  register_symbol("pc-multi-get-host-player-id", &pc_multi_get_host_player_id);
  register_symbol("pc-multi-get-player-ping", &pc_multi_get_player_ping);
  register_symbol("pc-multi-get-local-player-character", &pc_multi_get_local_player_character);
  register_symbol("pc-multi-disconnect", &pc_multi_disconnect);
  register_symbol("pc-multi-reconnect", &pc_multi_reconnect);
  register_symbol("pc-multi-get-command-line-arg", &pc_multi_get_command_line_arg);
}
