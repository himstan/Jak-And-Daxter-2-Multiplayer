#include "game/external/steam/steam_client.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>

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
struct ControllerInput {
  void* input = nullptr;
  bool (*init)(void*, bool) = nullptr;
  bool (*shutdown)(void*) = nullptr;
  bool (*set_manifest)(void*, const char*) = nullptr;
  void (*run_frame)(void*, bool) = nullptr;
  uint64_t (*action_set)(void*, const char*) = nullptr;
  void (*activate_action_set)(void*, uint64_t, uint64_t) = nullptr;
  uint64_t (*controller_for_index)(void*, int) = nullptr;
  int (*connected_controllers)(void*, uint64_t*) = nullptr;
  bool (*show_binding_panel)(void*, uint64_t) = nullptr;
  int (*input_type)(void*, uint64_t) = nullptr;
  void (*set_led)(void*, uint64_t, uint8_t, uint8_t, uint8_t, unsigned int) = nullptr;
  uint32_t (*remote_session)(void*, uint64_t) = nullptr;
  bool attempted = false;
  bool started = false;
  uint64_t gamepad_action_set = 0;
};

struct SteamClient {
  void* library = nullptr;
  void (*shutdown)() = nullptr;
  const char* (*persona_name)(void*) = nullptr;
  Networking networking;
  SteamApi api;
  ControllerInput controllers;
};

std::optional<SteamClient> g_client;
std::mutex g_controller_layout_mutex;
std::unordered_map<uint64_t, bool> g_controller_layouts;

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

bool resolve_controller_entry_points(void* library,
                                     SteamEntryPoints& entry,
                                     ControllerInput& controllers) {
  return resolve(library, entry.input, {"SteamAPI_SteamInput_v007", "SteamAPI_SteamInput_v006"}) &&
         resolve(library, controllers.init, {"SteamAPI_ISteamInput_Init"}) &&
         resolve(library, controllers.shutdown, {"SteamAPI_ISteamInput_Shutdown"}) &&
         resolve(library, controllers.set_manifest,
                 {"SteamAPI_ISteamInput_SetInputActionManifestFilePath"}) &&
         resolve(library, controllers.run_frame, {"SteamAPI_ISteamInput_RunFrame"}) &&
         resolve(library, controllers.action_set, {"SteamAPI_ISteamInput_GetActionSetHandle"}) &&
         resolve(library, controllers.activate_action_set,
                 {"SteamAPI_ISteamInput_ActivateActionSet"}) &&
         resolve(library, controllers.controller_for_index,
                 {"SteamAPI_ISteamInput_GetControllerForGamepadIndex"}) &&
         resolve(library, controllers.connected_controllers,
                 {"SteamAPI_ISteamInput_GetConnectedControllers"}) &&
         resolve(library, controllers.show_binding_panel,
                 {"SteamAPI_ISteamInput_ShowBindingPanel"}) &&
         resolve(library, controllers.input_type, {"SteamAPI_ISteamInput_GetInputTypeForHandle"}) &&
         resolve(library, controllers.set_led, {"SteamAPI_ISteamInput_SetLEDColor"}) &&
         resolve(library, controllers.remote_session,
                 {"SteamAPI_ISteamInput_GetRemotePlaySessionID"});
}

bool start_client(SteamClient& client, const SteamEntryPoints& entry, const uint32_t app_id) {
  set_environment("SteamAppId", std::to_string(app_id));
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
  if (entry.input) {
    client.controllers.input = entry.input();
  }
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
  if (!resolve_controller_entry_points(client.library, entry, client.controllers))
    entry.input = nullptr;
  if (!start_client(client, entry, app_id)) {
    close_library(client.library);
    return false;
  }
  g_client = client;
  g_client->api.dispatch_callback = detail::dispatch_callback;
  lg::info("[Steam] Steam is running this game as app {}.", app_id);
  return true;
}

void shutdown() {
  if (!g_client)
    return;
  if (g_client->controllers.started)
    g_client->controllers.shutdown(g_client->controllers.input);
  g_client->shutdown();
  g_client.reset();
  const std::lock_guard lock(g_controller_layout_mutex);
  g_controller_layouts.clear();
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

void update_controller_input() {
  if (!g_client || !g_client->controllers.input)
    return;
  auto& controllers = g_client->controllers;
  if (!controllers.attempted) {
    controllers.attempted = true;
    const auto manifest = fs::absolute(file_util::get_jak_project_dir() / "game" / "assets" /
                                       "steam_input" / "actions.vdf");
    if (!controllers.set_manifest(controllers.input, manifest.string().c_str())) {
      lg::warn("[Steam] Steam Input rejected the bundled gamepad layouts at {}.",
               manifest.string());
    }
    controllers.started = controllers.init(controllers.input, true);
  }
  if (controllers.started) {
    controllers.run_frame(controllers.input, false);
    if (!controllers.gamepad_action_set)
      controllers.gamepad_action_set = controllers.action_set(controllers.input, "Gamepad");
    if (controllers.gamepad_action_set)
      controllers.activate_action_set(controllers.input, UINT64_MAX,
                                      controllers.gamepad_action_set);
  }
}

uint64_t controller_for_gamepad_index(const int index) {
  if (!g_client || !g_client->controllers.started || index < 0)
    return 0;
  return g_client->controllers.controller_for_index(g_client->controllers.input, index);
}

std::string controller_name(const uint64_t handle) {
  if (!handle || !g_client || !g_client->controllers.started)
    return "";
  constexpr std::array<const char*, 15> names = {"Controller",
                                                 "Steam Controller",
                                                 "Xbox 360 Controller",
                                                 "Xbox One Controller",
                                                 "Gamepad",
                                                 "PS4 Controller",
                                                 "MFi Controller",
                                                 "Android Controller",
                                                 "Joy-Con Pair",
                                                 "Joy-Con",
                                                 "Switch Pro Controller",
                                                 "Touch Controller",
                                                 "PS3 Controller",
                                                 "PS5 Controller",
                                                 "Steam Deck"};
  const auto& controllers = g_client->controllers;
  const int type = controllers.input_type(controllers.input, handle);
  const char* name = type >= 0 && type < static_cast<int>(names.size()) ? names[type] : names[0];
  return std::string(name) + " (Steam Input)";
}

bool controller_has_led(const uint64_t handle) {
  if (!handle || !g_client || !g_client->controllers.started)
    return false;
  const auto& controllers = g_client->controllers;
  const int type = controllers.input_type(controllers.input, handle);
  return type == 5 || type == 13;  // ESteamInputType: PS4, PS5
}

void set_controller_led(const uint64_t handle,
                        const uint8_t red,
                        const uint8_t green,
                        const uint8_t blue) {
  if (controller_has_led(handle)) {
    const auto& controllers = g_client->controllers;
    controllers.set_led(controllers.input, handle, red, green, blue, 0);
  }
}

bool is_remote_controller(const uint64_t handle) {
  return handle && g_client && g_client->controllers.started &&
         g_client->controllers.remote_session(g_client->controllers.input, handle) != 0;
}

std::vector<uint64_t> controllers_without_gamepad_layout() {
  if (!g_client || !g_client->controllers.started)
    return {};
  const auto& controllers = g_client->controllers;
  std::array<uint64_t, 16> connected{};
  const int count = controllers.connected_controllers(controllers.input, connected.data());
  std::vector<uint64_t> result;
  for (int index = 0; index < count && index < static_cast<int>(connected.size()); ++index) {
    if (detail::controller_layout_uses_gamepad(connected[index]) == false)
      result.push_back(connected[index]);
  }
  return result;
}

bool show_controller_settings(const uint64_t handle) {
  return handle && g_client && g_client->controllers.started &&
         g_client->controllers.show_binding_panel(g_client->controllers.input, handle);
}

void detail::dispatch_callback(const CallbackMessage& message) {
  if (message.id != InputConfigurationLoaded::kId || !message.data ||
      message.size < static_cast<int>(sizeof(InputConfigurationLoaded)))
    return;
  InputConfigurationLoaded configuration;
  std::memcpy(&configuration, message.data, sizeof(configuration));
  if (configuration.app_id == kSpacewarAppId && configuration.controller) {
    const std::lock_guard lock(g_controller_layout_mutex);
    g_controller_layouts[configuration.controller] = configuration.uses_gamepad;
  }
}

std::optional<bool> detail::controller_layout_uses_gamepad(const uint64_t handle) {
  const std::lock_guard lock(g_controller_layout_mutex);
  const auto found = g_controller_layouts.find(handle);
  return found == g_controller_layouts.end() ? std::nullopt : std::optional(found->second);
}

const detail::SteamApi* detail::steam_api() {
  return g_client ? &g_client->api : nullptr;
}

}  // namespace steam
