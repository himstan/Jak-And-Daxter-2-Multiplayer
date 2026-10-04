#include "game/multiplayer/jak2/api/preferences.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/platform/profile/profile_store.h"
#include "game/multiplayer/platform/runtime/multiplayer_runtime.h"
#include "game/runtime.h"

namespace {
constexpr std::string_view kSettingsFileName = "multiplayer-profile.json";

MultiplayerPreferences g_preferences;
std::optional<multiplayer::platform::ProfileLease> g_profile_lease;
std::optional<fs::path> g_profile_root;

fs::path settings_path() {
  if (g_profile_lease)
    return g_profile_lease->game_preferences_path();
  const auto path = multiplayer::platform::multiplayer_runtime().game_preferences_path();
  if (path.empty())
    throw std::runtime_error("multiplayer profile has not been acquired");
  return path;
}

void acquire_profile() {
  if (g_profile_lease)
    return;
  if (!g_profile_root)
    return;
  g_profile_lease = multiplayer::platform::ProfileLease::acquire(
      {.game_id = "jak2", .root_directory = *g_profile_root, .maximum_instances = kMPMaxPlayers});
  if (!g_profile_lease) {
    throw std::runtime_error("could not acquire a multiplayer profile slot");
  }
}

bool load_common_profile(multiplayer::platform::StoredPlayerProfile& profile) {
  return g_profile_lease ? multiplayer::platform::load_player_profile(*g_profile_lease, profile)
                         : multiplayer::platform::multiplayer_runtime().load_profile(profile);
}

bool save_common_profile(const multiplayer::platform::StoredPlayerProfile& profile) {
  return g_profile_lease ? multiplayer::platform::save_player_profile(*g_profile_lease, profile)
                         : multiplayer::platform::multiplayer_runtime().save_profile(profile);
}

bool command_line_port(uint16_t& output) {
  for (int index = 1; index + 1 < g_argc; ++index) {
    if (g_argv[index] && g_argv[index + 1] && std::string_view(g_argv[index]) == "-mp-port") {
      return multiplayer::platform::parse_network_port(g_argv[index + 1], output);
    }
  }
  return false;
}

bool command_line_room_code(std::string& output) {
  for (int index = 1; index + 1 < g_argc; ++index) {
    if (g_argv[index] && g_argv[index + 1] && std::string_view(g_argv[index]) == "-mp-room-code") {
      return multiplayer::platform::normalize_room_code(g_argv[index + 1], output, false);
    }
  }
  return false;
}

void save_after_edit() {
  try {
    save_multiplayer_preferences();
  } catch (const std::exception& error) {
    lg::error("[Multiplayer] Could not save multiplayer settings: {}", error.what());
  }
}

int hex_digit(const char value) {
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

uint8_t color_channel(const float value) {
  return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}
}  // namespace
bool normalize_player_name(const std::string_view input,
                           std::string& output,
                           const bool allow_empty) {
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

bool parse_player_color(const std::string_view input, uint32_t& output) {
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

std::string format_player_color(uint32_t color_rgb) {
  std::string result(7, '0');
  result[0] = '#';
  for (int index = 6; index >= 1; --index) {
    constexpr char kHexDigits[] = "0123456789ABCDEF";
    result[index] = kHexDigits[color_rgb & 0xf];
    color_rgb >>= 4;
  }
  return result;
}

uint32_t generate_player_color() {
  static std::mt19937 generator(std::random_device{}());
  std::uniform_real_distribution<float> hue_distribution(0.0f, 360.0f);
  const float hue = hue_distribution(generator);
  constexpr float saturation = 0.85f;
  constexpr float value = 1.0f;
  constexpr float chroma = value * saturation;
  const float hue_sector = hue / 60.0f;
  const float second = chroma * (1.0f - std::fabs(std::fmod(hue_sector, 2.0f) - 1.0f));
  constexpr float match = value - chroma;
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

namespace {

bool parse_texture_group(const json& group, uint32_t& color, float& strength) {
  if (!group.is_object() || !group.contains("color") || !group.at("color").is_string() ||
      !group.contains("tint_strength") || !group.at("tint_strength").is_number() ||
      !parse_player_color(group.at("color").get<std::string>(), color))
    return false;
  strength = group.at("tint_strength").get<float>();
  return std::isfinite(strength) && strength >= 0.0f && strength <= 1.0f;
}

void parse_preferences_root(const json& root, MultiplayerPreferences& parsed, bool& needs_save) {
  needs_save |= root.contains("session_characters");
  try {
    if (root.contains("network_port") && root.at("network_port").is_number_unsigned()) {
      if (const uint32_t port = root.at("network_port").get<uint32_t>();
          multiplayer::platform::is_port_valid(port)) {
        parsed.network_port = static_cast<uint16_t>(port);
      }
    }
    if (root.contains("room_code") && root.at("room_code").is_string()) {
      if (std::string normalized; multiplayer::platform::normalize_room_code(
              root.at("room_code").get<std::string>(), normalized)) {
        parsed.room_code = std::move(normalized);
      }
    }
    uint32_t primary_color = kInvalidMultiplayerPlayerColor;
    if (root.contains("player_color") && root.at("player_color").is_string()) {
      parse_player_color(root.at("player_color").get<std::string>(), primary_color);
    }
    parsed.player_appearance = get_default_player_appearance(primary_color);
    if (!root.contains("player_texture_groups") || !root.at("player_texture_groups").is_object()) {
      needs_save = true;
    } else {
      const auto& texture_groups = root.at("player_texture_groups");
      for (const auto& definition : kMPPlayerTextureGroups) {
        const std::string key(definition.preference_key);
        if (!texture_groups.contains(key)) {
          needs_save = true;
          continue;
        }
        uint32_t group_color = 0;
        float group_strength = 0.0f;
        if (!parse_texture_group(texture_groups.at(key), group_color, group_strength)) {
          needs_save = true;
          continue;
        }
        const size_t slot = player_appearance_group_index(definition.group);
        parsed.player_appearance.colors[slot] = group_color;
        parsed.player_appearance.strengths[slot] = group_strength;
      }
    }
    if (root.contains("automatic_port_mapping") && root.at("automatic_port_mapping").is_boolean()) {
      parsed.automatic_port_mapping = root.at("automatic_port_mapping").get<bool>();
    }
    if (root.contains("player_collision") && root.at("player_collision").is_boolean()) {
      parsed.player_collision = root.at("player_collision").get<bool>();
    }
    if (root.contains("friendly_fire") && root.at("friendly_fire").is_boolean()) {
      parsed.friendly_fire = root.at("friendly_fire").get<bool>();
    }
    if (root.contains("player_map_marker") && root.at("player_map_marker").is_boolean()) {
      parsed.player_map_marker = root.at("player_map_marker").get<bool>();
    }
    if (root.contains("nametag_visibility") && root.at("nametag_visibility").is_number_unsigned() &&
        root.at("nametag_visibility") <= static_cast<uint8_t>(PlayerNametagVisibility::OFF)) {
      parsed.nametag_visibility =
          static_cast<PlayerNametagVisibility>(root.at("nametag_visibility").get<uint8_t>());
    }
    if (root.contains("respawn_delay_seconds") &&
        root.at("respawn_delay_seconds").is_number_unsigned() &&
        root.at("respawn_delay_seconds") <= std::numeric_limits<uint16_t>::max()) {
      parsed.respawn_delay_seconds = root.at("respawn_delay_seconds").get<uint16_t>();
    }
    if (root.contains("session_player_limit") &&
        root.at("session_player_limit").is_number_unsigned()) {
      if (const uint32_t limit = root.at("session_player_limit").get<uint32_t>();
          limit >= 2 && limit <= kMPMaxPlayers) {
        parsed.session_player_limit = static_cast<uint8_t>(limit);
      }
    }
  } catch (const std::exception& error) {
    lg::warn("[Multiplayer] Ignoring invalid multiplayer settings: {}", error.what());
  }
}

}  // namespace

MultiplayerPreferences parse_multiplayer_preferences(const std::string_view contents) {
  MultiplayerPreferences parsed;
  try {
    bool needs_save = false;
    const json root = parse_commented_json(std::string(contents), std::string(kSettingsFileName));
    parse_preferences_root(root, parsed, needs_save);
  } catch (const std::exception& error) {
    lg::warn("[Multiplayer] Ignoring invalid multiplayer settings: {}", error.what());
  }
  return parsed;
}

void load_multiplayer_preferences() {
  g_preferences = {};
  bool needs_save = false;
  try {
    acquire_profile();
    multiplayer::platform::StoredPlayerProfile identity;
    if (!load_common_profile(identity)) {
      throw std::runtime_error("invalid multiplayer identity profile");
    }
    g_preferences.player_name = identity.display_name;
    const auto path = settings_path();
    if (!file_util::file_exists(path.string())) {
      g_preferences.player_appearance = get_default_player_appearance(generate_player_color());
      save_after_edit();
      return;
    }
    lg::info("Loading multiplayer settings at {}", path.string());
    const std::string contents = file_util::read_text_file(path);
    const json root = parse_commented_json(contents, std::string(kSettingsFileName));
    g_preferences = {};
    parse_preferences_root(root, g_preferences, needs_save);
    g_preferences.player_name = identity.display_name;
  } catch (const std::exception& error) {
    g_preferences = {};
    lg::error("[Multiplayer] Could not load multiplayer settings: {}", error.what());
  }
  auto& [colors, strengths] = g_preferences.player_appearance;
  constexpr size_t primary_slot = player_appearance_group_index(MPPlayerAppearanceGroup::PRIMARY);
  if ((colors[primary_slot] & 0xff000000u) != 0) {
    colors[primary_slot] = generate_player_color();
    needs_save = true;
  }
  for (const auto& definition : kMPPlayerTextureGroups) {
    if (const size_t slot = player_appearance_group_index(definition.group);
        (colors[slot] & 0xff000000u) != 0 || !std::isfinite(strengths[slot]) ||
        strengths[slot] < 0.0f || strengths[slot] > 1.0f) {
      colors[slot] = colors[primary_slot];
      strengths[slot] = definition.group == MPPlayerAppearanceGroup::JAK_JACKET ||
                                definition.group == MPPlayerAppearanceGroup::DAXTER_HAT
                            ? 1.0f
                            : 0.0f;
      needs_save = true;
    }
  }
  for (size_t slot = 0; slot < kMPPlayerAppearanceSlotCount; ++slot) {
    if (!is_player_appearance_slot_registered(slot) &&
        (colors[slot] != colors[primary_slot] || strengths[slot] != 0.0f)) {
      colors[slot] = colors[primary_slot];
      strengths[slot] = 0.0f;
      needs_save = true;
    }
  }
  strengths[primary_slot] = 0.0f;
  if (needs_save) {
    save_after_edit();
  }
}

void save_multiplayer_preferences() {
  acquire_profile();
  json root;
  root["network_port"] = g_preferences.network_port;
  root["room_code"] = g_preferences.room_code;
  const auto& [colors, strengths] = g_preferences.player_appearance;
  root["player_color"] =
      format_player_color(colors[player_appearance_group_index(MPPlayerAppearanceGroup::PRIMARY)]);
  json texture_groups = json::object();
  for (const auto& definition : kMPPlayerTextureGroups) {
    const size_t slot = player_appearance_group_index(definition.group);
    texture_groups[std::string(definition.preference_key)] = {
        {"color", format_player_color(colors[slot])},
        {"tint_strength", strengths[slot]},
    };
  }
  root["player_texture_groups"] = std::move(texture_groups);
  root["player_collision"] = g_preferences.player_collision;
  root["friendly_fire"] = g_preferences.friendly_fire;
  root["player_map_marker"] = g_preferences.player_map_marker;
  root["nametag_visibility"] = static_cast<uint8_t>(g_preferences.nametag_visibility);
  root["respawn_delay_seconds"] = g_preferences.respawn_delay_seconds;
  root["automatic_port_mapping"] = g_preferences.automatic_port_mapping;
  root["session_player_limit"] = g_preferences.session_player_limit;
  const auto path = settings_path();
  file_util::create_dir_if_needed_for_file(path);
  file_util::write_text_file(path, root.dump(2));
  multiplayer::platform::StoredPlayerProfile identity;
  if (!load_common_profile(identity)) {
    throw std::runtime_error("could not load multiplayer identity profile");
  }
  identity.display_name = g_preferences.player_name;
  if (!save_common_profile(identity)) {
    throw std::runtime_error("could not save multiplayer identity profile");
  }
}

void set_multiplayer_preferences_root(fs::path root) {
  g_profile_lease.reset();
  g_profile_root = std::move(root);
}

void reset_multiplayer_preferences() {
  const std::string player_name = g_preferences.player_name;
  const MPPlayerAppearance player_appearance = g_preferences.player_appearance;
  g_preferences = {};
  g_preferences.player_name = player_name;
  g_preferences.player_appearance = player_appearance;
  save_after_edit();
}

const MultiplayerPreferences& multiplayer_preferences() {
  return g_preferences;
}

uint16_t get_resolved_host_port() {
  if (uint16_t override_port = 0; command_line_port(override_port)) {
    return override_port;
  }
  return g_preferences.network_port;
}

std::string get_resolved_host_room_code() {
  if (std::string cli_code; command_line_room_code(cli_code)) {
    return cli_code;
  }
  if (std::string normalized;
      multiplayer::platform::normalize_room_code(g_preferences.room_code, normalized, false)) {
    return normalized;
  }
  return multiplayer::platform::generate_room_code();
}

MultiplayerPreferences get_multiplayer_preferences() {
  auto preferences = g_preferences;
  multiplayer::platform::StoredPlayerProfile identity;
  load_common_profile(identity);
  preferences.preferred_character = identity.preferred_character;
  return preferences;
}

bool can_edit_multiplayer_preferences(const MultiplayerPreferences& previous,
                                      const MultiplayerPreferences& next,
                                      const multiplayer::platform::SessionState& session) {
  using multiplayer::platform::SessionRole;
  if (session.role == SessionRole::NONE)
    return true;
  if (previous.network_port != next.network_port || previous.room_code != next.room_code ||
      previous.session_player_limit != next.session_player_limit ||
      previous.automatic_port_mapping != next.automatic_port_mapping)
    return false;
  return session.role == SessionRole::HOST ||
         (previous.friendly_fire == next.friendly_fire &&
          previous.player_collision == next.player_collision &&
          previous.respawn_delay_seconds == next.respawn_delay_seconds);
}

bool set_multiplayer_preferences(MultiplayerPreferences preferences) {
  std::string name;
  std::string room_code;
  if (!multiplayer::platform::is_port_valid(preferences.network_port) ||
      !normalize_player_name(preferences.player_name, name) ||
      !multiplayer::platform::normalize_room_code(preferences.room_code, room_code) ||
      !is_player_appearance_valid(preferences.player_appearance) ||
      preferences.nametag_visibility > PlayerNametagVisibility::OFF ||
      preferences.session_player_limit < 2 || preferences.session_player_limit > kMPMaxPlayers ||
      (preferences.preferred_character != PlayerCharacter::JAK &&
       preferences.preferred_character != PlayerCharacter::DAXTER)) {
    return false;
  }
  multiplayer::platform::StoredPlayerProfile identity;
  if (!load_common_profile(identity)) {
    return false;
  }
  if (identity.preferred_character != preferences.preferred_character) {
    identity.preferred_character = preferences.preferred_character;
    if (!save_common_profile(identity)) {
      return false;
    }
  }
  preferences.player_name = std::move(name);
  preferences.room_code = std::move(room_code);
  g_preferences = std::move(preferences);
  save_after_edit();
  return true;
}

bool set_player_appearance(const MPPlayerAppearance& appearance) {
  if (!is_player_appearance_valid(appearance)) {
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
