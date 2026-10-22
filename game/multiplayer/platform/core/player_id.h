#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace multiplayer::platform {

struct PlayerId {
  std::array<uint8_t, 16> bytes = {};

  static PlayerId generate();
  static std::optional<PlayerId> parse(std::string_view text);
  std::string to_string() const;
  bool valid() const;
  bool operator==(const PlayerId&) const = default;
};
static_assert(sizeof(PlayerId) == 16);

}  // namespace multiplayer::platform
