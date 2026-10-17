#include "game/external/steam/steam_client.h"

#include <cstdlib>
#include <initializer_list>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/external/steam/steam_bindings.h"
#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

namespace steam {
namespace {
using namespace detail;

#ifdef _WIN32
constexpr const char* kLibraryName = "steam_api64.dll";
#elif defined(__APPLE__)
constexpr const char* kLibraryName = "libsteam_api.dylib";
#else
constexpr const char* kLibraryName = "libsteam_api.so";
#endif
constexpr int kInitOk = 0;
constexpr size_t kInitErrorBytes = 1024;

struct SteamClient {
  void* library = nullptr;
  void (*shutdown)() = nullptr;
  const char* (*persona_name)(void*) = nullptr;
  Networking networking;
  SteamApi api;
  InputApi input;
};

std::optional<SteamClient> g_client;

void* open_library() {
  const auto path = fs::path(file_util::get_current_executable_path()).parent_path() / kLibraryName;
#ifdef _WIN32
  return LoadLibraryW(path.wstring().c_str());
#else
  return dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void close_library(void* library) {
#ifdef _WIN32
  FreeLibrary(static_cast<HMODULE>(library));
#else
  dlclose(library);
#endif
}

void* find_symbol(void* library, const char* name) {
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
  return dlsym(library, name);
#endif
}

template <typename Function>
bool resolve(void* library, Function& function, const std::initializer_list<const char*> names) {
  for (const char* name : names) {
    if (void* symbol = find_symbol(library, name)) {
      function = reinterpret_cast<Function>(symbol);
      return true;
    }
  }
  lg::warn("[Steam] {} does not export {}.", kLibraryName, *names.begin());
  return false;
}

void set_environment(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

struct SteamEntryPoints {
  void (*dispatch_init)() = nullptr;
  int (*init)(char*) = nullptr;
  HSteamUser (*user)() = nullptr;
  void* (*find_interface)(HSteamUser, const char*) = nullptr;
  void* (*matchmaking)() = nullptr;
  void* (*friends)() = nullptr;
  void* (*utils)() = nullptr;
  uint32_t (*app_id)(void*) = nullptr;
  void* (*input)() = nullptr;
};

bool resolve_entry_points(void* library, SteamEntryPoints& entry, SteamClient& client) {
  auto& api = client.api;
  return resolve(library, entry.dispatch_init, {"SteamAPI_ManualDispatch_Init"}) &&
         resolve(library, entry.init, {"SteamAPI_InitFlat"}) &&
         resolve(library, client.shutdown, {"SteamAPI_Shutdown"}) &&
         resolve(library, client.persona_name, {"SteamAPI_ISteamFriends_GetPersonaName"}) &&
         resolve(library, entry.user, {"SteamAPI_GetHSteamUser"}) &&
         resolve(library, entry.find_interface, {"SteamInternal_FindOrCreateUserInterface"}) &&
         resolve(library, entry.matchmaking, {"SteamAPI_SteamMatchmaking_v009"}) &&
         resolve(library, entry.friends,
                 {"SteamAPI_SteamFriends_v018", "SteamAPI_SteamFriends_v017"}) &&
         resolve(library, entry.utils, {"SteamAPI_SteamUtils_v011", "SteamAPI_SteamUtils_v010"}) &&
         resolve(library, entry.app_id, {"SteamAPI_ISteamUtils_GetAppID"}) &&
         resolve(library, api.pipe, {"SteamAPI_GetHSteamPipe"}) &&
         resolve(library, api.run_frame, {"SteamAPI_ManualDispatch_RunFrame"}) &&
         resolve(library, api.next_callback, {"SteamAPI_ManualDispatch_GetNextCallback"}) &&
         resolve(library, api.free_callback, {"SteamAPI_ManualDispatch_FreeLastCallback"}) &&
         resolve(library, api.call_result, {"SteamAPI_ManualDispatch_GetAPICallResult"}) &&
         resolve(library, api.create_lobby, {"SteamAPI_ISteamMatchmaking_CreateLobby"}) &&
         resolve(library, api.set_lobby_data, {"SteamAPI_ISteamMatchmaking_SetLobbyData"}) &&
         resolve(library, api.lobby_data, {"SteamAPI_ISteamMatchmaking_GetLobbyData"}) &&
         resolve(library, api.request_lobby_data,
                 {"SteamAPI_ISteamMatchmaking_RequestLobbyData"}) &&
         resolve(library, api.leave_lobby, {"SteamAPI_ISteamMatchmaking_LeaveLobby"}) &&
         resolve(library, api.add_string_filter,
                 {"SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter"}) &&
         resolve(library, api.add_distance_filter,
                 {"SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter"}) &&
         resolve(library, api.add_result_count_filter,
                 {"SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter"}) &&
         resolve(library, api.request_lobby_list,
                 {"SteamAPI_ISteamMatchmaking_RequestLobbyList"}) &&
         resolve(library, api.lobby_by_index, {"SteamAPI_ISteamMatchmaking_GetLobbyByIndex"}) &&
         resolve(library, api.invite_dialog,
                 {"SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog"}) &&
         resolve(library, api.friend_relationship,
                 {"SteamAPI_ISteamFriends_GetFriendRelationship"}) &&
         resolve(library, api.set_lobby_type, {"SteamAPI_ISteamMatchmaking_SetLobbyType"}) &&
         resolve(library, api.join_lobby, {"SteamAPI_ISteamMatchmaking_JoinLobby"}) &&
         resolve(library, api.friend_count, {"SteamAPI_ISteamFriends_GetFriendCount"}) &&
         resolve(library, api.friend_by_index, {"SteamAPI_ISteamFriends_GetFriendByIndex"}) &&
         resolve(library, api.friend_game_played, {"SteamAPI_ISteamFriends_GetFriendGamePlayed"});
}

bool resolve_input_entry_points(void* library, SteamEntryPoints& entry, InputApi& input) {
  return resolve(library, entry.input, {"SteamAPI_SteamInput_v007", "SteamAPI_SteamInput_v006"}) &&
         resolve(library, input.init, {"SteamAPI_ISteamInput_Init"}) &&
         resolve(library, input.shutdown, {"SteamAPI_ISteamInput_Shutdown"}) &&
         resolve(library, input.set_manifest,
                 {"SteamAPI_ISteamInput_SetInputActionManifestFilePath"}) &&
         resolve(library, input.run_frame, {"SteamAPI_ISteamInput_RunFrame"}) &&
         resolve(library, input.connected_controllers,
                 {"SteamAPI_ISteamInput_GetConnectedControllers"}) &&
         resolve(library, input.action_set, {"SteamAPI_ISteamInput_GetActionSetHandle"}) &&
         resolve(library, input.activate_action_set, {"SteamAPI_ISteamInput_ActivateActionSet"}) &&
         resolve(library, input.digital_action, {"SteamAPI_ISteamInput_GetDigitalActionHandle"}) &&
         resolve(library, input.digital_data, {"SteamAPI_ISteamInput_GetDigitalActionData"}) &&
         resolve(library, input.analog_action, {"SteamAPI_ISteamInput_GetAnalogActionHandle"}) &&
         resolve(library, input.analog_data, {"SteamAPI_ISteamInput_GetAnalogActionData"}) &&
         resolve(library, input.vibrate, {"SteamAPI_ISteamInput_TriggerVibration"}) &&
         resolve(library, input.digital_origins,
                 {"SteamAPI_ISteamInput_GetDigitalActionOrigins"}) &&
         resolve(library, input.analog_origins, {"SteamAPI_ISteamInput_GetAnalogActionOrigins"}) &&
         resolve(library, input.show_binding_panel, {"SteamAPI_ISteamInput_ShowBindingPanel"}) &&
         resolve(library, input.input_type, {"SteamAPI_ISteamInput_GetInputTypeForHandle"}) &&
         resolve(library, input.set_led, {"SteamAPI_ISteamInput_SetLEDColor"});
}

// useful for steam deck if gk is added as a non steam game and run like that
bool launched_as_other_app(const uint32_t app_id) {
  const char* launched_game = std::getenv("SteamGameId");
  return launched_game && *launched_game && launched_game != std::to_string(app_id);
}

bool start_client(SteamClient& client, const SteamEntryPoints& entry, const uint32_t app_id) {
  const bool use_steam_input = entry.input && !launched_as_other_app(app_id);
  set_environment("SteamAppId", std::to_string(app_id));
  set_environment("SteamGameId", std::to_string(app_id));
  char error[kInitErrorBytes] = {};
  if (entry.init(error) != kInitOk) {
    lg::info("[Steam] Steam is unavailable ({}); using direct connections.", error);
    return false;
  }
  entry.dispatch_init();
  const auto user = entry.user();
  client.networking.sockets = static_cast<ISteamNetworkingSockets*>(
      entry.find_interface(user, STEAMNETWORKINGSOCKETS_INTERFACE_VERSION));
  client.networking.utils = static_cast<ISteamNetworkingUtils*>(
      entry.find_interface(user, STEAMNETWORKINGUTILS_INTERFACE_VERSION));
  client.api.matchmaking = entry.matchmaking();
  client.api.friends = entry.friends();
  void* utils = entry.utils();
  if (!client.networking.sockets || !client.networking.utils || !client.api.matchmaking ||
      !client.api.friends || !utils) {
    lg::warn("[Steam] Steam interfaces are unavailable; using direct connections.");
    client.shutdown();
    return false;
  }
  if (const auto running_app = entry.app_id(utils); running_app != app_id) {
    lg::warn("[Steam] Steam reports app {} instead of {}; using direct connections.", running_app,
             app_id);
    client.shutdown();
    return false;
  }
  client.networking.utils->InitRelayNetworkAccess();
  client.networking.sockets->InitAuthentication();
  if (use_steam_input)
    client.input.input = entry.input();
  else if (entry.input)
    lg::info("[Steam] Launched by Steam as another app; its controller layout drives input.");
  return true;
}
}  // namespace

bool initialize(const uint32_t app_id) {
  if (g_client)
    return true;
  SteamClient client;
  client.library = open_library();
  if (!client.library) {
    lg::info("[Steam] {} was not found next to the executable; using direct connections.",
             kLibraryName);
    return false;
  }
  SteamEntryPoints entry;
  if (!resolve_entry_points(client.library, entry, client)) {
    close_library(client.library);
    return false;
  }
  if (!resolve_input_entry_points(client.library, entry, client.input))
    entry.input = nullptr;
  if (!start_client(client, entry, app_id)) {
    close_library(client.library);
    return false;
  }
  g_client = client;
  lg::info("[Steam] Steam is running this game as app {}.", app_id);
  return true;
}

void shutdown() {
  if (!g_client)
    return;
  shutdown_input();
  g_client->shutdown();
  g_client.reset();
}

std::optional<Networking> networking() {
  if (!g_client)
    return std::nullopt;
  const auto& client_networking = g_client->networking;
  if (client_networking.sockets->GetAuthenticationStatus(nullptr) < 0 ||
      client_networking.utils->GetRelayNetworkStatus(nullptr) < 0) {
    return std::nullopt;
  }
  return client_networking;
}

std::string persona_name() {
  const char* name = g_client ? g_client->persona_name(g_client->api.friends) : nullptr;
  return name ? name : "";
}

uint64_t local_steam_id() {
  const auto client_networking = networking();
  SteamNetworkingIdentity identity;
  return client_networking && client_networking->sockets->GetIdentity(&identity)
             ? identity.GetSteamID64()
             : 0;
}

const detail::SteamApi* detail::steam_api() {
  return g_client ? &g_client->api : nullptr;
}

const detail::InputApi* detail::input_api() {
  return g_client && g_client->input.input ? &g_client->input : nullptr;
}

}  // namespace steam
