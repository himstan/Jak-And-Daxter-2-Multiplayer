#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "game/multiplayer/platform/runtime/runtime_command.h"

namespace multiplayer::platform {

struct DraftConnectionRequest {
  std::optional<ControllerClientConfig> connection;
  std::optional<DiscoveryRequest> discovery;
};

bool build_connection_request(std::string_view invite,
                              PlayerProfile profile,
                              ControllerClientConfig& request);

class ConnectionDraft {
 public:
  void clear();
  void reset(uint16_t default_port);
  std::string field(int field) const;
  bool set_field(int field, std::string_view value);
  bool ready() const;
  bool build_direct(PlayerProfile profile,
                    uint16_t discovery_port,
                    DraftConnectionRequest& request) const;
  bool stage(const std::string& invite);
  void clear_staged();
  bool staged() const { return staged_connection_.has_value(); }
  bool build_staged(PlayerProfile profile, ControllerClientConfig& request) const;

 private:
  bool direct_target(std::string& address, uint16_t& port, std::string& room_code) const;

  std::string address_;
  uint16_t port_ = 0;
  std::string room_code_;
  std::optional<ControllerClientConfig> staged_connection_;
};

}  // namespace multiplayer::platform
