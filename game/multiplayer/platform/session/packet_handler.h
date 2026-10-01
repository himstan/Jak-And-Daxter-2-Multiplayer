#pragma once

#include "game/multiplayer/platform/session/game_adapter.h"

namespace multiplayer::platform {

class PacketHandler {
 public:
  explicit PacketHandler(GameMessagePolicy policy) : policy_(policy) {}
  virtual ~PacketHandler() = default;
  const GameMessagePolicy& policy() const { return policy_; }
  virtual ValidatedPayload receive(const GameplayMessage&, GameSessionEndpoint&) = 0;
  virtual void publish(GameSessionEndpoint&, uint64_t now_ms) = 0;

 private:
  const GameMessagePolicy policy_;
};

}  // namespace multiplayer::platform
