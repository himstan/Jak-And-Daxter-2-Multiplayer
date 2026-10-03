#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/util/FileUtil.h"

#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/platform/session/connection_input.h"

inline constexpr uint32_t kInvalidMultiplayerPlayerColor = 0xffffffffu;

struct MultiplayerPreferences {
  uint16_t network_port = multiplayer::platform::kDefaultMultiplayerPort;
  std::string room_code;
  std::string player_name;
  MPPlayerAppearance player_appearance =
      get_default_player_appearance(kInvalidMultiplayerPlayerColor);
  bool automatic_port_mapping = true;
  uint32_t session_player_limit = 2;
};

bool normalize_player_name(std::string_view input, std::string& output, bool allow_empty = true);
bool parse_player_color(std::string_view input, uint32_t& output);
std::string format_player_color(uint32_t color_rgb);
uint32_t generate_player_color();
MultiplayerPreferences parse_multiplayer_preferences(std::string_view contents);

void load_multiplayer_preferences();
void set_multiplayer_preferences_root(fs::path root);
void save_multiplayer_preferences();
void reset_multiplayer_preferences();
const MultiplayerPreferences& multiplayer_preferences();
uint16_t get_resolved_host_port();
std::string get_resolved_host_room_code();

std::string get_multiplayer_preference_display(int field);
bool set_multiplayer_preference(int field, std::string_view value);
bool set_automatic_port_mapping(bool enabled);
bool set_player_appearance(const MPPlayerAppearance& appearance);
bool set_room_code_preference(std::string_view room_code);

uint32_t get_session_player_limit_preference();
bool set_session_player_limit_preference(uint32_t limit);
uint32_t get_player_character_preference();
bool set_player_character_preference(uint32_t character);
