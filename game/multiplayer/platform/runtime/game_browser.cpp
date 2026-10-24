#include "game/multiplayer/platform/runtime/game_browser.h"

#include <algorithm>
#include <functional>

namespace multiplayer::platform {

std::vector<GameListing> merge_game_listings(const std::span<const DiscoveryResult> lan,
                                             const std::span<const SteamLobbyListing> steam,
                                             const std::string_view compatibility_identity) {
  std::vector<GameListing> games;
  std::vector<std::string> lan_rooms;
  for (const auto& [invite, host] : lan) {
    const auto lobby = std::ranges::find(
        steam, host.room_code, [](const auto& listing) { return listing.metadata.room_code; });
    games.push_back({.host_name = host.host_name,
                     .details = host.details,
                     .players = host.current_players,
                     .player_limit = host.player_limit,
                     .friend_host = lobby != steam.end() && lobby->friend_host,
                     .compatible = host.compatibility_identity == compatibility_identity,
                     .invite = invite});
    lan_rooms.push_back(host.room_code);
  }
  for (const auto& [lobby_id, lobby, friend_host] : steam) {
    if (std::ranges::find(lan_rooms, lobby.room_code) != lan_rooms.end())
      continue;
    games.push_back({.host_name = lobby.host_name,
                     .details = lobby.details,
                     .players = lobby.players,
                     .player_limit = lobby.player_limit,
                     .friend_host = friend_host,
                     .compatible = lobby.compatibility_identity == compatibility_identity,
                     .lobby_id = lobby_id});
  }
  std::stable_partition(games.begin(), games.end(), std::mem_fn(&GameListing::friend_host));
  std::stable_partition(games.begin(), games.end(), std::mem_fn(&GameListing::compatible));
  return games;
}

DiscoveryStatus browser_status(const DiscoveryStatus lan,
                               const DiscoveryStatus steam,
                               const bool found_games) {
  if (lan == DiscoveryStatus::SEARCHING || steam == DiscoveryStatus::SEARCHING)
    return DiscoveryStatus::SEARCHING;
  if (found_games)
    return DiscoveryStatus::FOUND;
  return lan == DiscoveryStatus::IDLE && steam == DiscoveryStatus::IDLE
             ? DiscoveryStatus::IDLE
             : DiscoveryStatus::TIMED_OUT;
}

}  // namespace multiplayer::platform
