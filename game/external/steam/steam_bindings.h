#pragma once

#include <cstdint>
#include <optional>

#include "steam/steam_api_common.h"

namespace steam::detail {

using SteamApiCall = uint64_t;

#if defined(VALVE_CALLBACK_PACK_SMALL)
#pragma pack(push, 4)
#else
#pragma pack(push, 8)
#endif
struct CallbackMessage {
  HSteamUser user;
  int id;
  uint8_t* data;
  int size;
};

struct InputConfigurationLoaded {
  static constexpr int kId = 2803;
  uint32_t app_id;
  uint64_t controller;
  uint64_t creator;
  uint32_t major_revision;
  uint32_t minor_revision;
  bool uses_actions;
  bool uses_gamepad;
};

struct ApiCallCompleted {
  static constexpr int kId = 703;
  SteamApiCall call;
  int id;
  uint32_t size;
};

struct LobbyCreated {
  static constexpr int kId = 513;
  int result;
  uint64_t lobby;
};

struct LobbyMatchList {
  static constexpr int kId = 510;
  uint32_t count;
};

struct LobbyDataUpdate {
  static constexpr int kId = 505;
  uint64_t lobby;
  uint64_t member;
  uint8_t success;
};

struct LobbyJoinRequested {
  static constexpr int kId = 333;
  uint64_t lobby;
  uint64_t friend_id;
};

struct LobbyEnter {
  static constexpr int kId = 504;
  uint64_t lobby;
  uint32_t permissions;
  bool locked;
  uint32_t response;
};

struct FriendGameInfo {
  uint64_t game_id;
  uint32_t game_ip;
  uint16_t game_port;
  uint16_t query_port;
  uint64_t lobby;
};
#pragma pack(pop)

inline constexpr int kResultOk = 1;
inline constexpr int kLobbyTypePrivate = 0;
inline constexpr int kLobbyTypeFriendsOnly = 1;
inline constexpr int kLobbyTypePublic = 2;
inline constexpr uint32_t kChatRoomEnterSuccess = 1;
inline constexpr int kFriendFlagImmediate = 4;
inline constexpr int kLobbyComparisonEqual = 0;
inline constexpr int kLobbyDistanceWorldwide = 3;
inline constexpr int kFriendRelationshipFriend = 3;

struct SteamApi {
  HSteamPipe (*pipe)() = nullptr;
  void (*run_frame)(HSteamPipe) = nullptr;
  bool (*next_callback)(HSteamPipe, CallbackMessage*) = nullptr;
  void (*free_callback)(HSteamPipe) = nullptr;
  void (*dispatch_callback)(const CallbackMessage&) = nullptr;
  bool (*call_result)(HSteamPipe, SteamApiCall, void*, int, int, bool*) = nullptr;
  void* matchmaking = nullptr;
  void* friends = nullptr;
  SteamApiCall (*create_lobby)(void*, int, int) = nullptr;
  bool (*set_lobby_data)(void*, uint64_t, const char*, const char*) = nullptr;
  const char* (*lobby_data)(void*, uint64_t, const char*) = nullptr;
  bool (*request_lobby_data)(void*, uint64_t) = nullptr;
  void (*leave_lobby)(void*, uint64_t) = nullptr;
  void (*add_string_filter)(void*, const char*, const char*, int) = nullptr;
  void (*add_distance_filter)(void*, int) = nullptr;
  void (*add_result_count_filter)(void*, int) = nullptr;
  SteamApiCall (*request_lobby_list)(void*) = nullptr;
  uint64_t (*lobby_by_index)(void*, int) = nullptr;
  void (*invite_dialog)(void*, uint64_t) = nullptr;
  void (*user_overlay)(void*, const char*, uint64_t) = nullptr;
  int (*friend_relationship)(void*, uint64_t) = nullptr;
  bool (*set_lobby_type)(void*, uint64_t, int) = nullptr;
  SteamApiCall (*join_lobby)(void*, uint64_t) = nullptr;
  int (*friend_count)(void*, int) = nullptr;
  uint64_t (*friend_by_index)(void*, int, int) = nullptr;
  bool (*friend_game_played)(void*, uint64_t, FriendGameInfo*) = nullptr;
};

const SteamApi* steam_api();
void dispatch_callback(const CallbackMessage& message);
std::optional<bool> controller_layout_uses_gamepad(uint64_t handle);

}  // namespace steam::detail
