#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/util/FileUtil.h"

#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/platform/session/connection_input.h"
#include "game/multiplayer/platform/session/session_state.h"

inline constexpr uint32_t kInvalidMultiplayerPlayerColor = 0xffffffffu;

enum class PlayerNametagVisibility : uint8_t { ALWAYS, HOLD, OFF };

struct MultiplayerPreferences {
  uint16_t network_port = multiplayer::platform::kDefaultMultiplayerPort;
  uint16_t respawn_delay_seconds = multiplayer::jak2::core::kDefaultRespawnDelaySeconds;
  std::string room_code;
  std::string player_name;
  MPPlayerSkin player_skin = get_default_player_skin(kInvalidMultiplayerPlayerColor);
  bool automatic_port_mapping = true;
  bool player_collision = false;
  bool friendly_fire = false;
  bool player_map_marker = true;
  PlayerNametagVisibility nametag_visibility = PlayerNametagVisibility::HOLD;
  uint8_t session_player_limit = 2;
  PlayerCharacter preferred_character = PlayerCharacter::JAK;
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

MultiplayerPreferences get_multiplayer_preferences();
bool set_multiplayer_preferences(MultiplayerPreferences preferences);
bool can_edit_multiplayer_preferences(const MultiplayerPreferences& previous,
                                      const MultiplayerPreferences& next,
                                      const multiplayer::platform::SessionState& session);
bool set_player_skin(const MPPlayerSkin& skin);
