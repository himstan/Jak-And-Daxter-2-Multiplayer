#pragma once

#include "game/multiplayer/platform/protocol/message_frame.h"

namespace multiplayer::platform {

enum class FrameSendResult : uint8_t {
  REJECTED,
  NO_RECIPIENTS,
  QUEUED,
};

template <typename Send, typename Close>
FrameSendResult submit_frame(std::span<const ConnectionId> recipients,
                             std::span<const uint8_t> frame,
                             const FrameKind kind,
                             TransportLane lane,
                             Send send,
                             Close close) {
  if (recipients.empty())
    return FrameSendResult::NO_RECIPIENTS;
  bool queued = false;
  for (const auto connection : recipients) {
    if (send(connection, frame, lane)) {
      queued = true;
    } else if (kind == FrameKind::GAMEPLAY && lane == TransportLane::GAMEPLAY_RELIABLE) {
      close(connection);
    }
  }
  return queued ? FrameSendResult::QUEUED : FrameSendResult::REJECTED;
}

}  // namespace multiplayer::platform
