#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace multiplayer::platform {

inline constexpr std::string_view kInviteScheme = "jadmp";
inline constexpr size_t kMaximumInviteLength = 128;

std::string make_invite(std::string_view address, uint16_t port, std::string_view room_code);
bool parse_invite(std::string_view invite,
                  std::string& address,
                  uint16_t& port,
                  std::string& room_code);

}  // namespace multiplayer::platform
