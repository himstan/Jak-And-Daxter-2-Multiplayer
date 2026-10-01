#include "game/multiplayer/platform/discovery/invite.h"

#include <charconv>
#include <limits>

#include "game/multiplayer/platform/session/connection_input.h"

namespace multiplayer::platform {

std::string make_invite(const std::string_view address,
                        const uint16_t port,
                        const std::string_view room_code) {
  if (address.empty() || port == 0 || room_code.size() != kMultiplayerRoomCodeLength)
    return {};
  return std::string(kInviteScheme) + "://" + std::string(address) + ":" + std::to_string(port) +
         "/" + std::string(room_code);
}

bool parse_invite(const std::string_view invite,
                  std::string& address,
                  uint16_t& port,
                  std::string& room_code) {
  if (invite.size() > kMaximumInviteLength)
    return false;
  const std::string prefix = std::string(kInviteScheme) + "://";
  if (!invite.starts_with(prefix))
    return false;
  const auto slash = invite.rfind('/');
  const auto colon = invite.rfind(':', slash);
  if (slash == std::string_view::npos || colon == std::string_view::npos || colon < prefix.size() ||
      slash + 1 + kMultiplayerRoomCodeLength != invite.size())
    return false;
  const auto parsed_address = invite.substr(prefix.size(), colon - prefix.size());
  const auto parsed_code = invite.substr(slash + 1);
  for (const char ch : parsed_code) {
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')))
      return false;
  }
  uint32_t parsed_port = 0;
  const auto port_text = invite.substr(colon + 1, slash - colon - 1);
  const auto [end, error] =
      std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed_port);
  if (error != std::errc() || end != port_text.data() + port_text.size() || parsed_port == 0 ||
      parsed_port > (std::numeric_limits<uint16_t>::max)() || parsed_address.empty()) {
    return false;
  }
  address.assign(parsed_address);
  port = static_cast<uint16_t>(parsed_port);
  room_code.assign(parsed_code);
  return true;
}

}  // namespace multiplayer::platform
