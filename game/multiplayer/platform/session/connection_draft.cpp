#include "game/multiplayer/platform/session/connection_draft.h"

#include "common/cross_sockets/XSocket.h"

#include "game/multiplayer/platform/discovery/invite.h"
#include "game/multiplayer/platform/session/connection_input.h"

namespace multiplayer::platform {
bool build_connection_request(const std::string_view invite,
                              ParticipantProfile profile,
                              ControllerClientConfig& request) {
  std::string address;
  std::string room_code;
  uint16_t port = 0;
  if (!parse_invite(invite, address, port, room_code))
    return false;
  request = {.endpoint = std::move(address),
             .port = port,
             .room_code = std::move(room_code),
             .local_profile = std::move(profile)};
  return true;
}

void ConnectionDraft::clear() {
  address_.clear();
  port_ = 0;
  room_code_.clear();
}

void ConnectionDraft::reset(const uint16_t default_port) {
  clear();
  port_ = default_port;
}

std::string ConnectionDraft::field(const int field) const {
  if (field == 0)
    return address_;
  if (field == 1)
    return port_ == 0 ? std::string() : std::to_string(port_);
  if (field == 2)
    return room_code_;
  return {};
}

bool ConnectionDraft::set_field(const int field, const std::string_view value) {
  if (field == 0) {
    const std::string address(value);
    in_addr binary_address = {};
    if (inet_pton(AF_INET, address.c_str(), &binary_address) != 1)
      return false;
    address_ = address;
    return true;
  }
  if (field == 1) {
    uint16_t port = 0;
    if (!parse_network_port(value, port))
      return false;
    port_ = port;
    return true;
  }
  if (field == 2) {
    std::string normalized;
    if (!normalize_room_code(value, normalized))
      return false;
    room_code_ = std::move(normalized);
    return true;
  }
  return false;
}

bool ConnectionDraft::direct_target(std::string& address,
                                    uint16_t& port,
                                    std::string& room_code) const {
  if (address_.empty() || !is_port_valid(port_))
    return false;
  port = port_;
  address = address_;
  room_code = room_code_;
  return true;
}

bool ConnectionDraft::ready() const {
  DraftConnectionRequest request;
  return build_direct({}, 0, request);
}

bool ConnectionDraft::build_direct(ParticipantProfile profile,
                                   const uint16_t discovery_port,
                                   DraftConnectionRequest& request) const {
  std::string address;
  std::string room_code;
  uint16_t port = 0;
  if (!direct_target(address, port, room_code))
    return false;
  request = {};
  if (room_code.empty())
    request.discovery = {.discovery_port = discovery_port,
                         .expected_game_port = port,
                         .directed_address = std::move(address),
                         .profile = std::move(profile)};
  else
    request.connection = {.endpoint = std::move(address),
                          .port = port,
                          .room_code = std::move(room_code),
                          .local_profile = std::move(profile)};
  return true;
}

bool ConnectionDraft::stage(const std::string& invite) {
  ControllerClientConfig request;
  if (!build_connection_request(invite, {}, request))
    return false;
  staged_connection_ = std::move(request);
  return true;
}

void ConnectionDraft::clear_staged() {
  staged_connection_.reset();
}

bool ConnectionDraft::build_staged(ParticipantProfile profile,
                                   ControllerClientConfig& request) const {
  if (!staged_connection_)
    return false;
  request = *staged_connection_;
  request.local_profile = std::move(profile);
  return true;
}

}  // namespace multiplayer::platform
