#include "game/multiplayer/platform/steam/steam_lobby.h"

#include <algorithm>
#include <charconv>
#include <cstring>

#include "common/log/log.h"

#include "game/external/steam/steam_bindings.h"
#include "game/external/steam/steam_client.h"
#include "game/multiplayer/platform/profile/profile_store.h"
#include "game/multiplayer/platform/session/connection_input.h"
#include "steam/isteamnetworkingsockets.h"

namespace multiplayer::platform {
namespace {
using namespace steam::detail;

constexpr const char* kGameKey = "game";
constexpr const char* kIdentityKey = "identity";
constexpr const char* kRoomKey = "room";
constexpr const char* kNameKey = "name";
constexpr const char* kHostKey = "host";
constexpr const char* kPlayersKey = "players";
constexpr const char* kLimitKey = "limit";
constexpr const char* kDetailsKey = "details";

template <typename Value>
bool parse_number(const std::string& text, Value& value) {
  const auto* end = text.data() + text.size();
  const auto [parsed, error] = std::from_chars(text.data(), end, value);
  return !text.empty() && error == std::errc() && parsed == end;
}

int lobby_type(const LobbyPrivacy privacy) {
  switch (privacy) {
    case LobbyPrivacy::FRIENDS_ONLY:
      return kLobbyTypeFriendsOnly;
    case LobbyPrivacy::INVITE_ONLY:
      return kLobbyTypePrivate;
    default:
      return kLobbyTypePublic;
  }
}

template <typename Callback>
std::optional<Callback> callback_body(const CallbackMessage& message) {
  if (message.size < static_cast<int>(sizeof(Callback)))
    return std::nullopt;
  Callback body;
  std::memcpy(&body, message.data, sizeof(body));
  return body;
}

template <typename Result>
std::optional<Result> call_result(const SteamApi& api, const uint64_t call) {
  Result result = {};
  bool failed = true;
  if (!api.call_result(api.pipe(), call, &result, sizeof(result), Result::kId, &failed) || failed)
    return std::nullopt;
  return result;
}
}  // namespace

std::vector<SteamLobbyField> encode_steam_lobby(const SteamLobbyMetadata& metadata) {
  return {{kGameKey, metadata.game_id},
          {kIdentityKey, metadata.compatibility_identity},
          {kRoomKey, metadata.room_code},
          {kNameKey, metadata.host_name},
          {kHostKey, std::to_string(metadata.host_id)},
          {kPlayersKey, std::to_string(metadata.players)},
          {kLimitKey, std::to_string(metadata.player_limit)},
          {kDetailsKey, metadata.details}};
}

std::optional<SteamLobbyMetadata> decode_steam_lobby(const SteamLobbyReader& read,
                                                     const std::string_view game_id) {
  SteamLobbyMetadata metadata = {.game_id = read(kGameKey),
                                 .compatibility_identity = read(kIdentityKey),
                                 .room_code = read(kRoomKey),
                                 .host_name = read(kNameKey),
                                 .details = read(kDetailsKey)};
  if (metadata.game_id != game_id || metadata.room_code.size() != kMultiplayerRoomCodeLength ||
      !valid_display_name(metadata.host_name) || !valid_game_details(metadata.details) ||
      !parse_number(read(kHostKey), metadata.host_id) || metadata.host_id == 0 ||
      !parse_number(read(kPlayersKey), metadata.players) ||
      !parse_number(read(kLimitKey), metadata.player_limit) || metadata.player_limit < 2 ||
      metadata.players == 0 || metadata.players > metadata.player_limit) {
    return std::nullopt;
  }
  return metadata;
}

SteamLobby::~SteamLobby() {
  leave();
}

bool SteamLobby::create(SteamLobbyMetadata metadata, const LobbyPrivacy privacy) {
  leave();
  const auto* api = steam_api();
  metadata.host_id = steam::local_steam_id();
  if (!api || metadata.host_id == 0)
    return false;
  privacy_ = privacy;
  create_call_ = api->create_lobby(api->matchmaking, lobby_type(privacy), metadata.player_limit);
  hosted_ = std::move(metadata);
  return create_call_ != 0;
}

void SteamLobby::set_privacy(const LobbyPrivacy privacy) {
  if (privacy_ == privacy)
    return;
  privacy_ = privacy;
  if (const auto* api = steam_api(); api && lobby_id_ != 0)
    api->set_lobby_type(api->matchmaking, lobby_id_, lobby_type(privacy));
}

void SteamLobby::update_players(const uint8_t players) {
  if (hosted_.players == players)
    return;
  hosted_.players = players;
  publish();
}

void SteamLobby::update_details(std::string details) {
  if (hosted_.details == details)
    return;
  hosted_.details = std::move(details);
  publish();
}

void SteamLobby::publish() const {
  const auto* api = steam_api();
  if (!api || lobby_id_ == 0)
    return;
  for (const auto& [key, value] : encode_steam_lobby(hosted_))
    api->set_lobby_data(api->matchmaking, lobby_id_, key, value.c_str());
}

bool SteamLobby::search(std::string game_id) {
  const auto* api = steam_api();
  listings_.clear();
  search_call_ = 0;
  if (api) {
    api->add_string_filter(api->matchmaking, kGameKey, game_id.c_str(), kLobbyComparisonEqual);
    api->add_distance_filter(api->matchmaking, kLobbyDistanceWorldwide);
    api->add_result_count_filter(api->matchmaking, static_cast<int>(kMaximumSteamLobbies));
    search_call_ = api->request_lobby_list(api->matchmaking);
  }
  search_game_id_ = std::move(game_id);
  request_friend_lobbies();
  search_status_ = search_call_ != 0 ? DiscoveryStatus::SEARCHING : DiscoveryStatus::FAILED;
  return search_call_ != 0;
}

bool SteamLobby::join(const uint64_t lobby_id, std::string game_id) {
  const auto* api = steam_api();
  join_target_.reset();
  invite_lobby_ = 0;
  join_call_ = api && lobby_id != 0 ? api->join_lobby(api->matchmaking, lobby_id) : 0;
  if (join_call_ == 0) {
    join_status_ = DiscoveryStatus::FAILED;
    return false;
  }
  join_game_id_ = std::move(game_id);
  join_status_ = DiscoveryStatus::SEARCHING;
  return true;
}

bool SteamLobby::invite_friends() const {
  const auto* api = steam_api();
  if (!api || lobby_id_ == 0)
    return false;
  api->invite_dialog(api->friends, lobby_id_);
  return true;
}

void SteamLobby::leave() {
  if (const auto* api = steam_api(); api && lobby_id_ != 0)
    api->leave_lobby(api->matchmaking, lobby_id_);
  hosted_ = {};
  lobby_id_ = 0;
  create_call_ = 0;
  create_failed_ = false;
  privacy_ = LobbyPrivacy::PUBLIC;
  join_call_ = 0;
  join_target_.reset();
  join_status_ = DiscoveryStatus::IDLE;
}

void SteamLobby::pump() {
  const auto* api = steam_api();
  if (!api)
    return;
  const auto pipe = api->pipe();
  api->run_frame(pipe);
  CallbackMessage message = {};
  while (api->next_callback(pipe, &message)) {
    if (api->dispatch_callback)
      api->dispatch_callback(message);
    if (message.id == ApiCallCompleted::kId) {
      if (const auto completed = callback_body<ApiCallCompleted>(message)) {
        if (completed->id == LobbyCreated::kId)
          handle_created(completed->call);
        else if (completed->id == LobbyMatchList::kId)
          handle_listed(completed->call);
        else if (completed->id == LobbyEnter::kId)
          handle_entered(completed->call);
      }
    } else if (message.id == LobbyDataUpdate::kId) {
      if (const auto update = callback_body<LobbyDataUpdate>(message);
          update && update->member == update->lobby) {
        handle_data_update(update->lobby, update->success != 0);
      }
    } else if (message.id == LobbyJoinRequested::kId) {
      if (const auto request = callback_body<LobbyJoinRequested>(message))
        invite_lobby_ = request->lobby;
    }
    api->free_callback(pipe);
  }
}

void SteamLobby::handle_created(const uint64_t call) {
  const auto& api = *steam_api();
  const auto created = call_result<LobbyCreated>(api, call);
  const bool succeeded = created && created->result == kResultOk;
  if (call != create_call_) {
    if (succeeded)
      api.leave_lobby(api.matchmaking, created->lobby);
    return;
  }
  create_call_ = 0;
  if (!succeeded) {
    lg::warn("[Steam] Lobby creation failed.");
    create_failed_ = true;
    return;
  }
  lobby_id_ = created->lobby;
  publish();
  lg::info("[Steam] Hosting lobby {}.", lobby_id_);
}

void SteamLobby::handle_listed(const uint64_t call) {
  if (call != search_call_)
    return;
  search_call_ = 0;
  const auto& api = *steam_api();
  const auto list = call_result<LobbyMatchList>(api, call);
  if (!list) {
    search_status_ = DiscoveryStatus::FAILED;
    return;
  }
  const auto count = std::min<size_t>(list->count, kMaximumSteamLobbies);
  for (size_t index = 0; index < count; ++index)
    add_listing(api.lobby_by_index(api.matchmaking, static_cast<int>(index)));
  search_status_ = DiscoveryStatus::FOUND;
}

void SteamLobby::request_friend_lobbies() {
  friend_lobbies_.clear();
  const auto* api = steam_api();
  if (!api)
    return;
  const int count = api->friend_count(api->friends, kFriendFlagImmediate);
  for (int index = 0; index < count && friend_lobbies_.size() < kMaximumSteamLobbies; ++index) {
    FriendGameInfo game = {};
    const auto friend_id = api->friend_by_index(api->friends, index, kFriendFlagImmediate);
    if (api->friend_game_played(api->friends, friend_id, &game) && game.lobby != 0 &&
        api->request_lobby_data(api->matchmaking, game.lobby)) {
      friend_lobbies_.push_back(game.lobby);
    }
  }
}

void SteamLobby::add_listing(const uint64_t lobby_id) {
  if (listings_.size() >= kMaximumSteamLobbies ||
      std::ranges::find(listings_, lobby_id, &SteamLobbyListing::lobby_id) != listings_.end())
    return;
  auto metadata = read(lobby_id, search_game_id_);
  if (!metadata || metadata->host_id == steam::local_steam_id())
    return;
  const bool friend_host = steam::is_friend(metadata->host_id);
  listings_.push_back({lobby_id, std::move(*metadata), friend_host});
}

void SteamLobby::handle_entered(const uint64_t call) {
  const auto& api = *steam_api();
  const auto entered = call_result<LobbyEnter>(api, call);
  const bool succeeded = entered && entered->response == kChatRoomEnterSuccess;
  const bool current = call == join_call_;
  if (current) {
    join_call_ = 0;
    join_target_ = succeeded ? read(entered->lobby, join_game_id_) : std::nullopt;
    join_status_ = join_target_ ? DiscoveryStatus::FOUND : DiscoveryStatus::FAILED;
  }
  if (succeeded)
    api.leave_lobby(api.matchmaking, entered->lobby);
}

void SteamLobby::handle_data_update(const uint64_t lobby_id, const bool success) {
  const auto pending = std::ranges::find(friend_lobbies_, lobby_id);
  if (pending == friend_lobbies_.end())
    return;
  friend_lobbies_.erase(pending);
  if (success)
    add_listing(lobby_id);
}

std::optional<SteamLobbyMetadata> SteamLobby::read(const uint64_t lobby_id,
                                                   const std::string_view game_id) const {
  const auto& api = *steam_api();
  return decode_steam_lobby(
      [&](const char* key) {
        const char* value = api.lobby_data(api.matchmaking, lobby_id, key);
        return value ? std::string(value) : std::string();
      },
      game_id);
}

}  // namespace multiplayer::platform
