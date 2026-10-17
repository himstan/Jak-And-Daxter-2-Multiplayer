#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/multiplayer/platform/discovery/discovery_service.h"
#include "game/multiplayer/platform/session/session_state.h"

namespace multiplayer::platform {

inline constexpr size_t kMaximumSteamLobbies = 32;

struct SteamLobbyMetadata {
  std::string game_id;
  std::string compatibility_identity;
  std::string room_code;
  std::string host_name;
  std::string details;
  uint64_t host_id = 0;
  uint8_t players = 0;
  uint8_t player_limit = 0;
};

struct SteamLobbyListing {
  uint64_t lobby_id = 0;
  SteamLobbyMetadata metadata;
  bool friend_host = false;
};

using SteamLobbyField = std::pair<const char*, std::string>;
using SteamLobbyReader = std::function<std::string(const char* key)>;

std::vector<SteamLobbyField> encode_steam_lobby(const SteamLobbyMetadata& metadata);
std::optional<SteamLobbyMetadata> decode_steam_lobby(const SteamLobbyReader& read,
                                                     std::string_view game_id);

class SteamLobby {
 public:
  ~SteamLobby();

  bool create(SteamLobbyMetadata metadata, LobbyPrivacy privacy);
  void set_privacy(LobbyPrivacy privacy);
  void update_players(uint8_t players);
  void update_details(std::string details);
  bool search(std::string game_id);
  bool join(uint64_t lobby_id, std::string game_id);
  bool invite_friends() const;
  void leave();
  void pump();

  bool creating() const { return create_call_ != 0; }
  bool hosting() const { return lobby_id_ != 0; }
  bool take_create_failure() { return std::exchange(create_failed_, false); }
  std::optional<SteamLobbyMetadata> take_join_target() { return std::exchange(join_target_, {}); }
  void fail_join() { join_status_ = DiscoveryStatus::FAILED; }
  uint64_t invite() const { return invite_lobby_; }
  DiscoveryStatus search_status() const { return search_status_; }
  DiscoveryStatus join_status() const { return join_status_; }
  const std::vector<SteamLobbyListing>& listings() const { return listings_; }

 private:
  void publish() const;
  void request_friend_lobbies();
  void add_listing(uint64_t lobby_id);
  void handle_created(uint64_t call);
  void handle_listed(uint64_t call);
  void handle_entered(uint64_t call);
  void handle_data_update(uint64_t lobby_id, bool success);
  std::optional<SteamLobbyMetadata> read(uint64_t lobby_id, std::string_view game_id) const;

  SteamLobbyMetadata hosted_;
  std::string search_game_id_;
  std::string join_game_id_;
  std::vector<SteamLobbyListing> listings_;
  std::vector<uint64_t> friend_lobbies_;
  std::optional<SteamLobbyMetadata> join_target_;
  uint64_t lobby_id_ = 0;
  uint64_t create_call_ = 0;
  uint64_t search_call_ = 0;
  uint64_t join_call_ = 0;
  uint64_t invite_lobby_ = 0;
  DiscoveryStatus search_status_ = DiscoveryStatus::IDLE;
  DiscoveryStatus join_status_ = DiscoveryStatus::IDLE;
  LobbyPrivacy privacy_ = LobbyPrivacy::PUBLIC;
  bool create_failed_ = false;
};

}  // namespace multiplayer::platform
