#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "game/multiplayer/platform/discovery/discovery_service.h"
#include "game/multiplayer/platform/steam/steam_lobby.h"

namespace multiplayer::platform {

struct GameListing {
  std::string host_name;
  std::string details;
  uint8_t players = 0;
  uint8_t player_limit = 0;
  bool friend_host = false;
  bool compatible = true;
  std::string invite;
  uint64_t lobby_id = 0;
};

struct BrowserSnapshot {
  DiscoveryStatus status = DiscoveryStatus::IDLE;
  std::vector<GameListing> games;
};

std::vector<GameListing> merge_game_listings(std::span<const DiscoveryResult> lan,
                                             std::span<const SteamLobbyListing> steam,
                                             std::string_view compatibility_identity);
DiscoveryStatus browser_status(DiscoveryStatus lan, DiscoveryStatus steam, bool found_games);

}  // namespace multiplayer::platform
