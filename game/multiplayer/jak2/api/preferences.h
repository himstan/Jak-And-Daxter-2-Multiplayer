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
enum class PlayerGlowVisibility : uint8_t { DISABLED, WALL, ALWAYS };

struct PlayerGlowPreferences {
  static constexpr uint16_t kMaxFadeMs = 2000;
  static constexpr uint16_t kMaxFadeDistance = 500;
  static constexpr uint8_t kMinWidth = 1;
  static constexpr uint8_t kMaxWidth = 64;
  static constexpr uint8_t kMaxPercent = 100;

  PlayerGlowVisibility mode = PlayerGlowVisibility::WALL;
  uint8_t width = 22;  // pixels at 1080p
  uint8_t rounding = 100;
  uint8_t blur = 50;
  uint16_t fade_in_ms = 200;
  uint16_t fade_out_ms = 100;
  uint16_t fade_distance = 0;  // meters, 0 = infinite

  bool valid() const;
  bool operator==(const PlayerGlowPreferences&) const = default;
};

enum class HostVisibility : uint8_t { PUBLIC, FRIENDS_ONLY, INVITE_ONLY, LAN_ONLY };

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
  PlayerGlowPreferences player_glow;
  uint8_t session_player_limit = 2;
  PlayerCharacter preferred_character = PlayerCharacter::JAK;
  HostVisibility host_visibility = HostVisibility::PUBLIC;
};

multiplayer::platform::LobbyPrivacy lobby_privacy(HostVisibility visibility);
bool normalize_player_name(std::string_view input, std::string& output, bool allow_empty = true);
std::string player_name_from_steam(std::string_view persona_name);
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
