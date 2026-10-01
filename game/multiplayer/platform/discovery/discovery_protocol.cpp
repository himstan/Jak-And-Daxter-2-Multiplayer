#include "game/multiplayer/platform/discovery/discovery_protocol.h"

#include <algorithm>
#include <array>
#include <utility>

#include "game/multiplayer/platform/session/connection_input.h"
#include "game/multiplayer/platform/session/session_state.h"

namespace multiplayer::platform {
namespace {
constexpr std::array<uint8_t, 4> kMagic = {'J', 'M', 'P', 'D'};
constexpr uint8_t kVersion = 1;
constexpr uint8_t kQuery = 1;
constexpr uint8_t kAdvertisement = 2;

void append_u16(std::vector<uint8_t>& bytes, const uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
}

bool append_string(std::vector<uint8_t>& bytes, const std::string_view value) {
  if (value.empty() || value.size() > UINT8_MAX)
    return false;
  bytes.push_back(static_cast<uint8_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
  return true;
}

bool header(const std::span<const uint8_t> bytes, const uint8_t kind, size_t& cursor) {
  if (bytes.size() < 6 || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
      bytes[4] != kVersion || bytes[5] != kind)
    return false;
  cursor = 6;
  return true;
}

bool read_u16(const std::span<const uint8_t> bytes, size_t& cursor, uint16_t& value) {
  if (cursor + 2 > bytes.size())
    return false;
  value = static_cast<uint16_t>(bytes[cursor] | (static_cast<uint16_t>(bytes[cursor + 1]) << 8));
  cursor += 2;
  return true;
}

bool read_string(const std::span<const uint8_t> bytes, size_t& cursor, std::string& value) {
  if (cursor >= bytes.size())
    return false;
  const size_t size = bytes[cursor++];
  if (size == 0 || cursor + size > bytes.size())
    return false;
  value.assign(reinterpret_cast<const char*>(bytes.data() + cursor), size);
  cursor += size;
  return true;
}
}  // namespace

std::vector<uint8_t> encode_discovery_query(const std::string_view game_id) {
  std::vector<uint8_t> bytes(kMagic.begin(), kMagic.end());
  bytes.push_back(kVersion);
  bytes.push_back(kQuery);
  if (!append_string(bytes, game_id))
    return {};
  return bytes;
}

bool decode_discovery_query(const std::span<const uint8_t> bytes, std::string& game_id) {
  size_t cursor = 0;
  return header(bytes, kQuery, cursor) && read_string(bytes, cursor, game_id) &&
         cursor == bytes.size();
}

std::vector<uint8_t> encode_discovery_advertisement(const DiscoveryAdvertisement& advertisement) {
  if (advertisement.game_port == 0 || advertisement.current_players == 0 ||
      !valid_player_limit(advertisement.player_limit) ||
      advertisement.current_players > advertisement.player_limit ||
      advertisement.room_code.size() != kMultiplayerRoomCodeLength ||
      advertisement.game_id.empty() || advertisement.compatibility_identity.empty())
    return {};
  std::vector<uint8_t> bytes(kMagic.begin(), kMagic.end());
  bytes.push_back(kVersion);
  bytes.push_back(kAdvertisement);
  append_u16(bytes, advertisement.game_port);
  bytes.push_back(advertisement.current_players);
  bytes.push_back(advertisement.player_limit);
  if (!append_string(bytes, advertisement.game_id) ||
      !append_string(bytes, advertisement.compatibility_identity) ||
      !append_string(bytes, advertisement.room_code))
    return {};
  return bytes;
}

bool decode_discovery_advertisement(const std::span<const uint8_t> bytes,
                                    DiscoveryAdvertisement& advertisement) {
  size_t cursor = 0;
  DiscoveryAdvertisement parsed;
  if (!header(bytes, kAdvertisement, cursor) || !read_u16(bytes, cursor, parsed.game_port) ||
      cursor + 2 > bytes.size())
    return false;
  parsed.current_players = bytes[cursor++];
  parsed.player_limit = bytes[cursor++];
  if (!read_string(bytes, cursor, parsed.game_id) ||
      !read_string(bytes, cursor, parsed.compatibility_identity) ||
      !read_string(bytes, cursor, parsed.room_code) || cursor != bytes.size() ||
      parsed.game_port == 0 || parsed.current_players == 0 ||
      parsed.room_code.size() != kMultiplayerRoomCodeLength ||
      !valid_player_limit(parsed.player_limit) || parsed.current_players > parsed.player_limit ||
      parsed.game_id.empty() || parsed.compatibility_identity.empty())
    return false;
  advertisement = std::move(parsed);
  return true;
}

}  // namespace multiplayer::platform
