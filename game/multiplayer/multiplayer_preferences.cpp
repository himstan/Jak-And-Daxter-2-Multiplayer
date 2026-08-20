#include "game/multiplayer/multiplayer_preferences.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <random>

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

int hex_digit(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

uint8_t color_channel(float value) {
  return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
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

bool mp_parse_player_color(std::string_view input, uint32_t& output) {
  if (input.size() != 7 || input.front() != '#') {
    return false;
  }
  uint32_t parsed = 0;
  for (size_t index = 1; index < input.size(); ++index) {
    const int digit = hex_digit(input[index]);
    if (digit < 0) {
      return false;
    }
    parsed = (parsed << 4) | static_cast<uint32_t>(digit);
  }
  output = parsed;
  return true;
}

std::string mp_format_player_color(uint32_t color_rgb) {
  constexpr char kHexDigits[] = "0123456789ABCDEF";
  std::string result(7, '0');
  result[0] = '#';
  for (int index = 6; index >= 1; --index) {
    result[index] = kHexDigits[color_rgb & 0xf];
    color_rgb >>= 4;
  }
  return result;
}

uint32_t mp_generate_vivid_player_color() {
  static std::mt19937 generator(std::random_device{}());
  std::uniform_real_distribution<float> hue_distribution(0.0f, 360.0f);
  const float hue = hue_distribution(generator);
  constexpr float saturation = 0.85f;
  constexpr float value = 1.0f;
  const float chroma = value * saturation;
  const float hue_sector = hue / 60.0f;
  const float second = chroma * (1.0f - std::fabs(std::fmod(hue_sector, 2.0f) - 1.0f));
  const float match = value - chroma;
  float red = 0.0f;
  float green = 0.0f;
  float blue = 0.0f;
  if (hue_sector < 1.0f) {
    red = chroma;
    green = second;
  } else if (hue_sector < 2.0f) {
    red = second;
    green = chroma;
  } else if (hue_sector < 3.0f) {
    green = chroma;
    blue = second;
  } else if (hue_sector < 4.0f) {
    green = second;
    blue = chroma;
  } else if (hue_sector < 5.0f) {
    red = second;
    blue = chroma;
  } else {
    red = chroma;
    blue = second;
  }
  return (static_cast<uint32_t>(color_channel(red + match)) << 16) |
         (static_cast<uint32_t>(color_channel(green + match)) << 8) |
         static_cast<uint32_t>(color_channel(blue + match));
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
    uint32_t primary_color = kInvalidMultiplayerPlayerColor;
    if (root.contains("player_color") && root.at("player_color").is_string()) {
      mp_parse_player_color(root.at("player_color").get<std::string>(), primary_color);
    }
    float legacy_strength = 1.0f;
    if (root.contains("player_tint_strength") && root.at("player_tint_strength").is_number()) {
      const float strength = root.at("player_tint_strength").get<float>();
      if (std::isfinite(strength) && strength >= 0.0f && strength <= 1.0f) {
        legacy_strength = strength;
      }
    }
    parsed.player_appearance = mp_default_player_appearance(primary_color, legacy_strength);
    if (root.contains("player_texture_groups") &&
        root.at("player_texture_groups").is_object()) {
      const auto& texture_groups = root.at("player_texture_groups");
      bool has_valid_leggings = false;
      bool has_valid_straps = false;
      for (const auto& definition : kMPPlayerTextureGroups) {
        const std::string key(definition.preference_key);
        if (!texture_groups.contains(key) || !texture_groups.at(key).is_object()) {
          continue;
        }
        const auto& group = texture_groups.at(key);
        if (!group.contains("color") || !group.at("color").is_string() ||
            !group.contains("tint_strength") || !group.at("tint_strength").is_number()) {
          continue;
        }
        uint32_t group_color = 0;
        const float group_strength = group.at("tint_strength").get<float>();
        if (!mp_parse_player_color(group.at("color").get<std::string>(), group_color) ||
            !std::isfinite(group_strength) || group_strength < 0.0f || group_strength > 1.0f) {
          continue;
        }
        const size_t slot = mp_player_appearance_group_index(definition.group);
        parsed.player_appearance.colors[slot] = group_color;
        parsed.player_appearance.strengths[slot] = group_strength;
        if (definition.group == MPPlayerAppearanceGroup::JAK_LEGGINGS) {
          has_valid_leggings = true;
        }
        if (definition.group == MPPlayerAppearanceGroup::JAK_STRAPS) {
          has_valid_straps = true;
        }
      }
      if (!has_valid_leggings) {
        const size_t leggings_slot =
            mp_player_appearance_group_index(MPPlayerAppearanceGroup::JAK_LEGGINGS);
        const size_t pants_slot =
            mp_player_appearance_group_index(MPPlayerAppearanceGroup::JAK_PANTS);
        parsed.player_appearance.colors[leggings_slot] =
            parsed.player_appearance.colors[pants_slot];
        parsed.player_appearance.strengths[leggings_slot] =
            parsed.player_appearance.strengths[pants_slot];
      }
      if (!has_valid_straps) {
        const size_t straps_slot =
            mp_player_appearance_group_index(MPPlayerAppearanceGroup::JAK_STRAPS);
        const size_t jacket_slot =
            mp_player_appearance_group_index(MPPlayerAppearanceGroup::JAK_JACKET);
        parsed.player_appearance.colors[straps_slot] =
            parsed.player_appearance.colors[jacket_slot];
        parsed.player_appearance.strengths[straps_slot] =
            parsed.player_appearance.strengths[jacket_slot];
      }
    }
    if (root.contains("automatic_port_mapping") &&
        root.at("automatic_port_mapping").is_boolean()) {
      parsed.automatic_port_mapping = root.at("automatic_port_mapping").get<bool>();
    }
    if (root.contains("session_player_limit") &&
        root.at("session_player_limit").is_number_unsigned()) {
      const uint32_t limit = root.at("session_player_limit").get<uint32_t>();
      if (limit >= 2 && limit <= kMPMaxPlayers) {
        parsed.session_player_limit = limit;
      }
    }
    if (root.contains("session_characters") && root.at("session_characters").is_array()) {
      const auto& chars = root.at("session_characters");
      for (size_t i = 0; i < kMPMaxPlayers && i < chars.size(); ++i) {
        if (chars[i].is_number_unsigned()) {
          const uint32_t val = chars[i].get<uint32_t>();
          if (val == static_cast<uint32_t>(MPPlayerCharacter::JAK) ||
              val == static_cast<uint32_t>(MPPlayerCharacter::DAXTER)) {
            parsed.session_characters[i] = static_cast<MPPlayerCharacter>(val);
          }
        }
      }
    }
  } catch (const std::exception& error) {
    lg::warn("[Multiplayer] Ignoring invalid multiplayer settings: {}", error.what());
  }
  return parsed;
}

void mp_load_multiplayer_preferences() {
  g_preferences = {};
  bool needs_save = false;
  try {
    const auto path = settings_path();
    if (!file_util::file_exists(path.string())) {
      g_preferences.player_appearance =
          mp_default_player_appearance(mp_generate_vivid_player_color());
      mp_discard_multiplayer_preference_edits();
      save_after_edit();
      return;
    }
    lg::info("Loading multiplayer settings at {}", path.string());
    const std::string contents = file_util::read_text_file(path);
    g_preferences = mp_parse_multiplayer_preferences(contents);
    const json root = parse_commented_json(contents, std::string(kSettingsFileName));
    if (!root.contains("player_texture_groups") ||
        !root.at("player_texture_groups").is_object()) {
      needs_save = true;
    } else {
      const auto& texture_groups = root.at("player_texture_groups");
      for (const auto& definition : kMPPlayerTextureGroups) {
        const std::string key(definition.preference_key);
        if (!texture_groups.contains(key) || !texture_groups.at(key).is_object()) {
          needs_save = true;
          continue;
        }
        const auto& group = texture_groups.at(key);
        uint32_t color = 0;
        if (!group.contains("color") || !group.at("color").is_string() ||
            !mp_parse_player_color(group.at("color").get<std::string>(), color) ||
            !group.contains("tint_strength") || !group.at("tint_strength").is_number()) {
          needs_save = true;
          continue;
        }
        const float strength = group.at("tint_strength").get<float>();
        if (!std::isfinite(strength) || strength < 0.0f || strength > 1.0f) {
          needs_save = true;
        }
      }
    }
  } catch (const std::exception& error) {
    g_preferences = {};
    lg::error("[Multiplayer] Could not load multiplayer settings: {}", error.what());
  }
  auto& appearance = g_preferences.player_appearance;
  const size_t primary_slot =
      mp_player_appearance_group_index(MPPlayerAppearanceGroup::PRIMARY);
  if ((appearance.colors[primary_slot] & 0xff000000u) != 0) {
    appearance.colors[primary_slot] = mp_generate_vivid_player_color();
    needs_save = true;
  }
  for (const auto& definition : kMPPlayerTextureGroups) {
    const size_t slot = mp_player_appearance_group_index(definition.group);
    if ((appearance.colors[slot] & 0xff000000u) != 0 ||
        !std::isfinite(appearance.strengths[slot]) || appearance.strengths[slot] < 0.0f ||
        appearance.strengths[slot] > 1.0f) {
      appearance.colors[slot] = appearance.colors[primary_slot];
      appearance.strengths[slot] =
          definition.group == MPPlayerAppearanceGroup::JAK_JACKET ||
                  definition.group == MPPlayerAppearanceGroup::DAXTER_HAT
              ? 1.0f
              : 0.0f;
      needs_save = true;
    }
  }
  for (size_t slot = 0; slot < kMPPlayerAppearanceSlotCount; ++slot) {
    if (!mp_player_appearance_slot_registered(slot) &&
        (appearance.colors[slot] != appearance.colors[primary_slot] ||
         appearance.strengths[slot] != 0.0f)) {
      appearance.colors[slot] = appearance.colors[primary_slot];
      appearance.strengths[slot] = 0.0f;
      needs_save = true;
    }
  }
  appearance.strengths[primary_slot] = 0.0f;
  mp_discard_multiplayer_preference_edits();
  if (needs_save) {
    save_after_edit();
  }
}

void mp_save_multiplayer_preferences() {
  json root;
  root["network_port"] = g_preferences.network_port;
  root["room_code"] = g_preferences.room_code;
  root["player_name"] = g_preferences.player_name;
  const auto& appearance = g_preferences.player_appearance;
  root["player_color"] = mp_format_player_color(
      appearance.colors[mp_player_appearance_group_index(MPPlayerAppearanceGroup::PRIMARY)]);
  json texture_groups = json::object();
  for (const auto& definition : kMPPlayerTextureGroups) {
    const size_t slot = mp_player_appearance_group_index(definition.group);
    texture_groups[std::string(definition.preference_key)] = {
        {"color", mp_format_player_color(appearance.colors[slot])},
        {"tint_strength", appearance.strengths[slot]},
    };
  }
  root["player_texture_groups"] = std::move(texture_groups);
  root["automatic_port_mapping"] = g_preferences.automatic_port_mapping;
  root["session_player_limit"] = g_preferences.session_player_limit;
  json chars_json = json::array();
  for (uint32_t i = 0; i < kMPMaxPlayers; ++i) {
    chars_json.push_back(static_cast<uint32_t>(g_preferences.session_characters[i]));
  }
  root["session_characters"] = chars_json;
  const auto path = settings_path();
  file_util::create_dir_if_needed_for_file(path);
  file_util::write_text_file(path, root.dump(2));
}

void mp_reset_multiplayer_preferences() {
  const std::string player_name = g_preferences.player_name;
  const MPPlayerAppearance player_appearance = g_preferences.player_appearance;
  g_preferences = {};
  g_preferences.player_name = player_name;
  g_preferences.player_appearance = player_appearance;
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

bool mp_set_player_appearance(const MPPlayerAppearance& appearance) {
  if (!mp_valid_player_appearance(appearance)) {
    return false;
  }
  if (g_preferences.player_appearance.colors == appearance.colors &&
      g_preferences.player_appearance.strengths == appearance.strengths) {
    return true;
  }
  g_preferences.player_appearance = appearance;
  save_after_edit();
  return true;
}

uint32_t mp_get_session_player_limit_preference() {
  return g_preferences.session_player_limit;
}

bool mp_set_session_player_limit_preference(uint32_t limit) {
  if (limit < 2 || limit > kMPMaxPlayers) {
    return false;
  }
  if (g_preferences.session_player_limit == limit) {
    return true;
  }
  g_preferences.session_player_limit = limit;
  save_after_edit();
  return true;
}

uint32_t mp_get_session_player_character_preference(uint32_t player_id) {
  if (player_id >= kMPMaxPlayers) {
    return static_cast<uint32_t>(MPPlayerCharacter::UNKNOWN);
  }
  return static_cast<uint32_t>(g_preferences.session_characters[player_id]);
}

bool mp_set_session_player_character_preference(uint32_t player_id, uint32_t character) {
  if (player_id >= kMPMaxPlayers) {
    return false;
  }
  if (character != static_cast<uint32_t>(MPPlayerCharacter::JAK) &&
      character != static_cast<uint32_t>(MPPlayerCharacter::DAXTER)) {
    return false;
  }
  const auto player_char = static_cast<MPPlayerCharacter>(character);
  if (g_preferences.session_characters[player_id] == player_char) {
    return true;
  }
  g_preferences.session_characters[player_id] = player_char;
  save_after_edit();
  return true;
}
