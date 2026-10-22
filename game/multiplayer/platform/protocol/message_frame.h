#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "game/multiplayer/platform/core/types.h"

namespace multiplayer::platform {

enum class FrameKind : uint8_t {
  CONTROL = 1,
  GAMEPLAY = 2,
  BOOTSTRAP = 3,
};

struct MessageFrame {
  FrameKind kind = FrameKind::GAMEPLAY;
  PlayerSlot origin = kInvalidPlayerSlot;
  std::span<const uint8_t> payload;
};

std::vector<uint8_t> encode_message_frame(FrameKind kind,
                                          PlayerSlot origin,
                                          std::span<const uint8_t> payload);
bool decode_message_frame(std::span<const uint8_t> bytes, MessageFrame& frame);

}  // namespace multiplayer::platform
