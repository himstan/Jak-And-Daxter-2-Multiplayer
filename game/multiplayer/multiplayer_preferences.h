#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "game/multiplayer/multiplayer_protocol.h"

inline constexpr uint16_t kDefaultMultiplayerPort = 26210;
inline constexpr uint16_t kMultiplayerDiscoveryPort = 26211;
inline constexpr size_t kMultiplayerRoomCodeLength = 6;
inline constexpr uint32_t kInvalidMultiplayerPlayerColor = 0xffffffffu;

struct MultiplayerPreferences {
  uint16_t network_port = kDefaultMultiplayerPort;
  std::string room_code;
  std::string player_name;
  MPPlayerAppearance player_appearance =
      mp_default_player_appearance(kInvalidMultiplayerPlayerColor);
  bool automatic_port_mapping = true;
  uint32_t session_player_limit = 2;
  std::array<MPPlayerCharacter, kMPMaxPlayers> session_characters =
      mp_default_player_character_config();
};

bool mp_valid_gameplay_port(uint32_t port);
bool mp_normalize_room_code(std::string_view input, std::string& output, bool allow_empty = true);
bool mp_normalize_player_name(std::string_view input, std::string& output, bool allow_empty = true);
bool mp_parse_player_color(std::string_view input, uint32_t& output);
std::string mp_format_player_color(uint32_t color_rgb);
uint32_t mp_generate_vivid_player_color();
MultiplayerPreferences mp_parse_multiplayer_preferences(std::string_view contents);

void mp_load_multiplayer_preferences();
void mp_save_multiplayer_preferences();
void mp_reset_multiplayer_preferences();
const MultiplayerPreferences& mp_multiplayer_preferences();
uint16_t mp_resolved_host_port();

std::string mp_multiplayer_preference_display(int field);
int mp_edit_multiplayer_preference(int field, uint32_t key);
bool mp_commit_multiplayer_preference(int field);
void mp_discard_multiplayer_preference_edits();
bool mp_set_automatic_port_mapping(bool enabled);
bool mp_set_player_appearance(const MPPlayerAppearance& appearance);

uint32_t mp_get_session_player_limit_preference();
bool mp_set_session_player_limit_preference(uint32_t limit);
uint32_t mp_get_session_player_character_preference(uint32_t player_id);
bool mp_set_session_player_character_preference(uint32_t player_id, uint32_t character);
