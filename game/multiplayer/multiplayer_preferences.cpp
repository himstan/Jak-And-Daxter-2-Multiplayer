#include "game/multiplayer/multiplayer_preferences.h"

#include <charconv>
#include <limits>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"
#include "game/runtime.h"
#include "game/multiplayer/multiplayer_protocol.h"

#include "third-party/SDL/include/SDL3/SDL.h"

namespace {
constexpr int kNetworkPortField = 0;
constexpr int kRoomCodeField = 1;
constexpr int kPlayerNameField = 2;
constexpr std::string_view kSettingsFileName = "multiplayer-settings.json";

MultiplayerPreferences g_preferences;
std::string g_port_draft = std::to_string(kDefaultMultiplayerPort);
std::string g_room_code_draft;
std::string g_player_name_draft;

fs::path settings_path() {
  return file_util::get_user_settings_dir(g_game_version) / std::string(kSettingsFileName);
}

bool parse_port(std::string_view text, uint16_t& output) {
  uint32_t parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size() ||
      !mp_valid_gameplay_port(parsed)) {
    return false;
  }
  output = static_cast<uint16_t>(parsed);
  return true;
}

std::string port_text() {
  return g_port_draft;
}

bool command_line_port(uint16_t& output) {
  for (int index = 1; index + 1 < g_argc; ++index) {
    if (g_argv[index] && g_argv[index + 1] && std::string_view(g_argv[index]) == "-mp-port") {
      return parse_port(g_argv[index + 1], output);
    }
  }
  return false;
}

void save_after_edit() {
  try {
    mp_save_multiplayer_preferences();
  } catch (const std::exception& error) {
    lg::error("[Multiplayer] Could not save multiplayer settings: {}", error.what());
  }
}
}  // namespace

bool mp_valid_gameplay_port(uint32_t port) {
  return port >= 1024 && port <= (std::numeric_limits<uint16_t>::max)() &&
         port != kMultiplayerDiscoveryPort;
}

bool mp_normalize_room_code(std::string_view input, std::string& output, bool allow_empty) {
  output.clear();
  if (input.empty()) {
    return allow_empty;
  }
  if (input.size() != kMultiplayerRoomCodeLength) {
    return false;
  }
  output.reserve(input.size());
  for (char character : input) {
    if (character >= 'a' && character <= 'z') {
      character = static_cast<char>(character - 'a' + 'A');
    }
    const bool valid = (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9');
    if (!valid) {
      output.clear();
      return false;
    }
    output.push_back(character);
  }
  return true;
}

bool mp_normalize_player_name(std::string_view input, std::string& output, bool allow_empty) {
  output.clear();
  if (input.empty()) {
    return allow_empty;
  }
  if (input.size() >= kMultiplayerPlayerNameSize) {
    return false;
  }
  for (const char character : input) {
    const bool valid = (character >= 'A' && character <= 'Z') ||
                       (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9');
    if (!valid) {
      output.clear();
      return false;
    }
  }
  output.assign(input);
  return true;
}

MultiplayerPreferences mp_parse_multiplayer_preferences(std::string_view contents) {
  MultiplayerPreferences parsed;
  try {
    const json root = parse_commented_json(std::string(contents), std::string(kSettingsFileName));
    if (root.contains("network_port") && root.at("network_port").is_number_unsigned()) {
      const uint32_t port = root.at("network_port").get<uint32_t>();
      if (mp_valid_gameplay_port(port)) {
        parsed.network_port = static_cast<uint16_t>(port);
      }
    }
    if (root.contains("room_code") && root.at("room_code").is_string()) {
      std::string normalized;
      if (mp_normalize_room_code(root.at("room_code").get<std::string>(), normalized)) {
        parsed.room_code = std::move(normalized);
      }
    }
    if (root.contains("player_name") && root.at("player_name").is_string()) {
      std::string normalized;
      if (mp_normalize_player_name(root.at("player_name").get<std::string>(), normalized)) {
        parsed.player_name = std::move(normalized);
      }
    }
    if (root.contains("automatic_port_mapping") &&
        root.at("automatic_port_mapping").is_boolean()) {
      parsed.automatic_port_mapping = root.at("automatic_port_mapping").get<bool>();
    }
  } catch (const std::exception& error) {
    lg::warn("[Multiplayer] Ignoring invalid multiplayer settings: {}", error.what());
  }
  return parsed;
}

void mp_load_multiplayer_preferences() {
  g_preferences = {};
  try {
    const auto path = settings_path();
    if (!file_util::file_exists(path.string())) {
      mp_discard_multiplayer_preference_edits();
      return;
    }
    lg::info("Loading multiplayer settings at {}", path.string());
    g_preferences = mp_parse_multiplayer_preferences(file_util::read_text_file(path));
  } catch (const std::exception& error) {
    g_preferences = {};
    lg::error("[Multiplayer] Could not load multiplayer settings: {}", error.what());
  }
  mp_discard_multiplayer_preference_edits();
}

void mp_save_multiplayer_preferences() {
  json root;
  root["network_port"] = g_preferences.network_port;
  root["room_code"] = g_preferences.room_code;
  root["player_name"] = g_preferences.player_name;
  root["automatic_port_mapping"] = g_preferences.automatic_port_mapping;
  const auto path = settings_path();
  file_util::create_dir_if_needed_for_file(path);
  file_util::write_text_file(path, root.dump(2));
}

void mp_reset_multiplayer_preferences() {
  const std::string player_name = g_preferences.player_name;
  g_preferences = {};
  g_preferences.player_name = player_name;
  mp_discard_multiplayer_preference_edits();
  save_after_edit();
}

const MultiplayerPreferences& mp_multiplayer_preferences() {
  return g_preferences;
}

uint16_t mp_resolved_host_port() {
  uint16_t override_port = 0;
  if (command_line_port(override_port)) {
    return override_port;
  }
  return g_preferences.network_port;
}

std::string mp_multiplayer_preference_display(int field) {
  if (field == kNetworkPortField) {
    return port_text();
  }
  if (field == kRoomCodeField) {
    return g_room_code_draft;
  }
  if (field == kPlayerNameField) {
    return g_player_name_draft;
  }
  return {};
}

int mp_edit_multiplayer_preference(int field, uint32_t key) {
  const bool backspace = key == SDLK_BACKSPACE;
  if (field == kNetworkPortField) {
    if (backspace) {
      if (!g_port_draft.empty()) {
        g_port_draft.pop_back();
      }
    } else if (key >= '0' && key <= '9' && g_port_draft.size() < 5) {
      g_port_draft.push_back(static_cast<char>(key));
    } else {
      return 0;
    }
    return 1;
  }

  if (field != kRoomCodeField) {
    if (field != kPlayerNameField) {
      return 0;
    }
    if (backspace) {
      if (g_player_name_draft.empty()) {
        return 0;
      }
      g_player_name_draft.pop_back();
      return 1;
    }
    char character = key <= 0x7f ? static_cast<char>(key) : '\0';
    const SDL_Keymod modifiers = SDL_GetModState();
    const bool uppercase = ((modifiers & SDL_KMOD_SHIFT) != 0) !=
                            ((modifiers & SDL_KMOD_CAPS) != 0);
    if (uppercase && character >= 'a' && character <= 'z') {
      character = static_cast<char>(character - 'a' + 'A');
    }
    const bool valid = (character >= 'A' && character <= 'Z') ||
                       (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9');
    if (!valid || g_player_name_draft.size() >= kMultiplayerPlayerNameSize - 1) {
      return 0;
    }
    g_player_name_draft.push_back(character);
    return 1;
  }
  if (backspace) {
    if (g_room_code_draft.empty()) {
      return 0;
    }
    g_room_code_draft.pop_back();
    return 1;
  }
  char character = key <= 0x7f ? static_cast<char>(key) : '\0';
  if (character >= 'a' && character <= 'z') {
    character = static_cast<char>(character - 'a' + 'A');
  }
  const bool valid = (character >= 'A' && character <= 'Z') ||
                     (character >= '0' && character <= '9');
  if (!valid || g_room_code_draft.size() >= kMultiplayerRoomCodeLength) {
    return 0;
  }
  g_room_code_draft.push_back(character);
  return 1;
}

bool mp_commit_multiplayer_preference(int field) {
  if (field == kNetworkPortField) {
    uint16_t port = 0;
    if (!parse_port(g_port_draft, port)) {
      return false;
    }
    g_preferences.network_port = port;
    save_after_edit();
    return true;
  }
  if (field == kRoomCodeField) {
    std::string normalized;
    if (!mp_normalize_room_code(g_room_code_draft, normalized)) {
      return false;
    }
    g_preferences.room_code = std::move(normalized);
    g_room_code_draft = g_preferences.room_code;
    save_after_edit();
    return true;
  }
  if (field == kPlayerNameField) {
    std::string normalized;
    if (!mp_normalize_player_name(g_player_name_draft, normalized)) {
      return false;
    }
    g_preferences.player_name = std::move(normalized);
    g_player_name_draft = g_preferences.player_name;
    save_after_edit();
    return true;
  }
  return false;
}

void mp_discard_multiplayer_preference_edits() {
  g_port_draft = std::to_string(g_preferences.network_port);
  g_room_code_draft = g_preferences.room_code;
  g_player_name_draft = g_preferences.player_name;
}

bool mp_set_automatic_port_mapping(bool enabled) {
  if (g_preferences.automatic_port_mapping == enabled) {
    return true;
  }
  g_preferences.automatic_port_mapping = enabled;
  save_after_edit();
  return true;
}
