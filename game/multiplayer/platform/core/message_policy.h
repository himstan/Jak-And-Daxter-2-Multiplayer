#pragma once

#include <cstdint>
#include <span>

#include "game/multiplayer/platform/core/types.h"

namespace multiplayer::platform {

enum class MessageDirection : uint8_t {
  HOST_TO_CLIENT,
  CLIENT_TO_HOST,
  BIDIRECTIONAL,
};

enum class CadenceMode : uint8_t {
  ON_DEMAND,
  DIRTY,
  PERIODIC,
};

enum class MessagePriority : uint8_t {
  CRITICAL,
  NORMAL,
  BULK,
};

enum class NetworkPressure : uint8_t {
  NORMAL,
  DEGRADED,
  CONGESTED,
  SEVERE,
};

struct GameMessagePolicy {
  uint8_t id = 0;
  const char* name = "";
  MessageDirection direction = MessageDirection::BIDIRECTIONAL;
  Delivery delivery = Delivery::UNRELIABLE_REALTIME;
  MessagePriority priority = MessagePriority::NORMAL;
  CadenceMode cadence = CadenceMode::ON_DEMAND;
  uint32_t interval_ms = 0;
  uint32_t maximum_payload_bytes = 0;
};

const GameMessagePolicy* find_message_policy(std::span<const GameMessagePolicy> policies,
                                             uint8_t message_id);
bool can_send_message(const GameMessagePolicy& policy, SessionRole role);
bool can_receive_message(const GameMessagePolicy& policy, SessionRole receiver_role);
bool validate_message_policies(std::span<const GameMessagePolicy> policies,
                               uint32_t logical_payload_limit);

}  // namespace multiplayer::platform
