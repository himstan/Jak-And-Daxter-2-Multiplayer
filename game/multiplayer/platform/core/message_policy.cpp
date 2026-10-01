#include "game/multiplayer/platform/core/message_policy.h"

#include <array>

namespace multiplayer::platform {

const GameMessagePolicy* find_message_policy(const std::span<const GameMessagePolicy> policies,
                                             const uint8_t message_id) {
  for (const auto& policy : policies) {
    if (policy.id == message_id)
      return &policy;
  }
  return nullptr;
}

bool can_send_message(const GameMessagePolicy& policy, const SessionRole role) {
  switch (policy.direction) {
    case MessageDirection::HOST_TO_CLIENT:
      return role == SessionRole::HOST;
    case MessageDirection::CLIENT_TO_HOST:
      return role == SessionRole::CLIENT;
    case MessageDirection::BIDIRECTIONAL:
      return role == SessionRole::HOST || role == SessionRole::CLIENT;
  }
  return false;
}

bool can_receive_message(const GameMessagePolicy& policy, const SessionRole receiver_role) {
  switch (policy.direction) {
    case MessageDirection::HOST_TO_CLIENT:
      return receiver_role == SessionRole::CLIENT;
    case MessageDirection::CLIENT_TO_HOST:
      return receiver_role == SessionRole::HOST;
    case MessageDirection::BIDIRECTIONAL:
      return receiver_role == SessionRole::HOST || receiver_role == SessionRole::CLIENT;
  }
  return false;
}

bool validate_message_policies(const std::span<const GameMessagePolicy> policies,
                               const uint32_t logical_payload_limit) {
  if (policies.empty() || logical_payload_limit == 0)
    return false;
  std::array<bool, 256> seen = {};
  for (const auto& policy : policies) {
    if (seen[policy.id] || !policy.name || policy.name[0] == '\0' ||
        policy.maximum_payload_bytes == 0 || policy.maximum_payload_bytes > logical_payload_limit ||
        (policy.delivery == Delivery::RELIABLE_ORDERED &&
         policy.priority != MessagePriority::NORMAL) ||
        (policy.cadence == CadenceMode::PERIODIC && policy.interval_ms == 0) ||
        (policy.cadence != CadenceMode::PERIODIC && policy.interval_ms != 0)) {
      return false;
    }
    seen[policy.id] = true;
  }
  return true;
}

}  // namespace multiplayer::platform
