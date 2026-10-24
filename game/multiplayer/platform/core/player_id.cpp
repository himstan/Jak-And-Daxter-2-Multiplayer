#include "game/multiplayer/platform/core/player_id.h"

#include <random>

namespace multiplayer::platform {
namespace {
constexpr std::string_view kHex = "0123456789abcdef";
}

PlayerId PlayerId::generate() {
  std::random_device entropy;
  PlayerId id;
  for (auto& byte : id.bytes)
    byte = static_cast<uint8_t>(entropy());
  id.bytes[6] = (id.bytes[6] & 0x0f) | 0x40;
  id.bytes[8] = (id.bytes[8] & 0x3f) | 0x80;
  return id;
}

bool PlayerId::valid() const {
  return (bytes[6] & 0xf0) == 0x40 && (bytes[8] & 0xc0) == 0x80;
}

std::string PlayerId::to_string() const {
  std::string text;
  text.reserve(36);
  for (size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10)
      text.push_back('-');
    text.push_back(kHex[bytes[index] >> 4]);
    text.push_back(kHex[bytes[index] & 0x0f]);
  }
  return text;
}

std::optional<PlayerId> PlayerId::parse(const std::string_view text) {
  if (text.size() != 36)
    return std::nullopt;
  PlayerId id;
  size_t cursor = 0;
  for (size_t index = 0; index < id.bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      if (text[cursor++] != '-')
        return std::nullopt;
    }
    const auto high = kHex.find(text[cursor++]);
    const auto low = kHex.find(text[cursor++]);
    if (high == std::string_view::npos || low == std::string_view::npos)
      return std::nullopt;
    id.bytes[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return id.valid() ? std::optional{id} : std::nullopt;
}

}  // namespace multiplayer::platform
