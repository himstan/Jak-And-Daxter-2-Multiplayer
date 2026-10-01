#include "game/multiplayer/platform/protocol/message_frame.h"

namespace multiplayer::platform {
namespace {
constexpr uint8_t kFrameVersion = 1;
constexpr size_t kFrameHeaderSize = 3;
}  // namespace

std::vector<uint8_t> encode_message_frame(const FrameKind kind,
                                          const PlayerId origin,
                                          const std::span<const uint8_t> payload) {
  if (origin == kInvalidPlayerId || payload.empty())
    return {};
  std::vector<uint8_t> result;
  result.reserve(kFrameHeaderSize + payload.size());
  result.push_back(kFrameVersion);
  result.push_back(static_cast<uint8_t>(kind));
  result.push_back(origin);
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}

bool decode_message_frame(const std::span<const uint8_t> bytes, MessageFrame& frame) {
  if (bytes.size() <= kFrameHeaderSize || bytes[0] != kFrameVersion ||
      bytes[1] < static_cast<uint8_t>(FrameKind::CONTROL) ||
      bytes[1] > static_cast<uint8_t>(FrameKind::BOOTSTRAP) || bytes[2] == kInvalidPlayerId) {
    return false;
  }
  frame.kind = static_cast<FrameKind>(bytes[1]);
  frame.origin = bytes[2];
  frame.payload = bytes.subspan(kFrameHeaderSize);
  return true;
}

}  // namespace multiplayer::platform
