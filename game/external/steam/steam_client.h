#pragma once

#include <cstdint>
#include <optional>
#include <string>

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

}  // namespace steam
