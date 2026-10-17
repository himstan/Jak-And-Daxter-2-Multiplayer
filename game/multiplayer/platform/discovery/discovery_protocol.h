#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace multiplayer::platform {

inline constexpr size_t kMaximumGameDetailsBytes = 32;

bool valid_game_details(std::string_view details);

struct DiscoveryAdvertisement {
  uint16_t game_port = 0;
  uint8_t current_players = 0;
  uint8_t player_limit = 0;
  std::string game_id;
  std::string compatibility_identity;
  std::string room_code;
  std::string host_name;
  std::string details;
};

std::vector<uint8_t> encode_discovery_query(std::string_view game_id);
bool decode_discovery_query(std::span<const uint8_t> bytes, std::string& game_id);
std::vector<uint8_t> encode_discovery_advertisement(const DiscoveryAdvertisement& advertisement);
bool decode_discovery_advertisement(std::span<const uint8_t> bytes,
                                    DiscoveryAdvertisement& advertisement);

}  // namespace multiplayer::platform
