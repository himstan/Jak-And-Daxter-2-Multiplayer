#include "game/multiplayer/platform/session/packet_registry.h"

#include <stdexcept>

namespace multiplayer::platform {

PacketRegistry::PacketRegistry(std::vector<std::unique_ptr<PacketHandler>> handlers,
                               uint32_t maximum_payload_bytes)
    : handlers_(std::move(handlers)) {
  policies_.reserve(handlers_.size());
  for (const auto& handler : handlers_) {
    if (!handler)
      throw std::invalid_argument("null packet handler");
    policies_.push_back(handler->policy());
  }
  if (!validate_message_policies(policies_, maximum_payload_bytes))
    throw std::invalid_argument("invalid packet registry");
}

const PacketHandler* PacketRegistry::find(uint8_t id) const {
  for (const auto& handler : handlers_)
    if (handler->policy().id == id)
      return handler.get();
  return nullptr;
}

ValidatedPayload PacketRegistry::receive(const GameplayMessage& message,
                                         GameSessionEndpoint& endpoint) {
  if (message.sequence == 0)
    return {};
  for (const auto& handler : handlers_)
    if (handler->policy().id == message.message_id)
      return handler->receive(message, endpoint);
  return {};
}

void PacketRegistry::publish(GameSessionEndpoint& endpoint, uint64_t now_ms) {
  for (const auto& handler : handlers_)
    handler->publish(endpoint, now_ms);
}

}  // namespace multiplayer::platform
