#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class ISteamNetworkingSockets;
class ISteamNetworkingUtils;

namespace steam {

inline constexpr uint32_t kSpacewarAppId = 480;

struct Networking {
  ISteamNetworkingSockets* sockets = nullptr;
  ISteamNetworkingUtils* utils = nullptr;
};

bool initialize(uint32_t app_id);
void shutdown();
std::optional<Networking> networking();
std::string persona_name();
uint64_t local_steam_id();
bool is_friend(uint64_t steam_id);
bool show_profile(uint64_t steam_id);
void update_controller_input();
uint64_t controller_for_gamepad_index(int index);
std::string controller_name(uint64_t handle);
bool controller_has_led(uint64_t handle);
void set_controller_led(uint64_t handle, uint8_t red, uint8_t green, uint8_t blue);
bool is_remote_controller(uint64_t handle);
std::vector<uint64_t> controllers_without_gamepad_layout();
bool show_controller_settings(uint64_t handle);

}  // namespace steam
