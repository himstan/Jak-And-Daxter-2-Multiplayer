#pragma once

#include <memory>

#include "game/multiplayer/platform/session/packet_handler.h"

namespace multiplayer::platform {

class PacketRegistry {
 public:
  explicit PacketRegistry(std::vector<std::unique_ptr<PacketHandler>> handlers,
                          uint32_t maximum_payload_bytes);
  const PacketHandler* find(uint8_t id) const;
  std::span<const GameMessagePolicy> policies() const { return policies_; }
  ValidatedPayload receive(const GameplayMessage&, GameSessionEndpoint&);
  void publish(GameSessionEndpoint&, uint64_t now_ms);

 private:
  const std::vector<std::unique_ptr<PacketHandler>> handlers_;
  std::vector<GameMessagePolicy> policies_;
};

}  // namespace multiplayer::platform
