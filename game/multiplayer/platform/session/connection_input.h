#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace multiplayer::platform {

inline constexpr uint16_t kDefaultMultiplayerPort = 26210;
inline constexpr uint16_t kMultiplayerDiscoveryPort = 26211;
inline constexpr size_t kMultiplayerRoomCodeLength = 6;

inline bool is_port_valid(const uint32_t port) {
  return port >= 1024 && port <= (std::numeric_limits<uint16_t>::max)() &&
         port != kMultiplayerDiscoveryPort;
}

inline bool parse_network_port(const std::string_view text, uint16_t& output) {
  uint32_t parsed = 0;
  if (const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
      text.empty() || ec != std::errc() || ptr != text.data() + text.size() ||
      !is_port_valid(parsed)) {
    return false;
  }
  output = static_cast<uint16_t>(parsed);
  return true;
}

inline bool normalize_room_code_character(char& character) {
  if (character >= 'a' && character <= 'z')
    character = static_cast<char>(character - 'a' + 'A');
  return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
}

inline bool normalize_room_code(const std::string_view input,
                                std::string& output,
                                const bool allow_empty = true) {
  output.clear();
  if (input.empty())
    return allow_empty;
  if (input.size() != kMultiplayerRoomCodeLength)
    return false;
  output.reserve(input.size());
  for (char character : input) {
    if (!normalize_room_code_character(character)) {
      output.clear();
      return false;
    }
    output.push_back(character);
  }
  return true;
}

std::string generate_room_code();

}  // namespace multiplayer::platform
