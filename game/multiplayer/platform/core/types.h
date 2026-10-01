#pragma once

#include <cstdint>

namespace multiplayer::platform {

using PlayerId = uint8_t;
using ConnectionId = uint32_t;

inline constexpr PlayerId kInvalidPlayerId = 0xff;

enum class PlayerCharacter : uint8_t {
  UNKNOWN = 0,
  JAK = 1,
  DAXTER = 2,
};

enum class SessionRole : uint8_t {
  NONE,
  HOST,
  CLIENT,
};

enum class Delivery : uint8_t {
  UNRELIABLE_REALTIME,
  RELIABLE_ORDERED,
};

enum class TransportLane : uint16_t {
  CONTROL_RELIABLE = 0,
  REALTIME_CRITICAL = 1,
  GAMEPLAY_RELIABLE = 2,
  REALTIME_NORMAL = 3,
  REALTIME_BULK = 4,
};

enum class AudienceKind : uint8_t {
  CONNECTION,
  EVERYONE,
  EVERYONE_EXCEPT_ORIGIN,
};

struct Audience {
  AudienceKind kind = AudienceKind::EVERYONE;
  ConnectionId connection_id = 0;

  static Audience one(const ConnectionId connection_id) {
    return {.kind = AudienceKind::CONNECTION, .connection_id = connection_id};
  }
  static Audience everyone() { return {}; }
  static Audience everyone_except_origin() {
    return {.kind = AudienceKind::EVERYONE_EXCEPT_ORIGIN};
  }
};

struct MessageOrigin {
  ConnectionId connection_id = 0;
  PlayerId authenticated_player_id = kInvalidPlayerId;
  bool from_host = false;
};

}  // namespace multiplayer::platform
