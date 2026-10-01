#include "game/multiplayer/platform/core/sequence.h"

namespace multiplayer::platform {

bool sequence_is_newer(const uint32_t incoming, const uint32_t previous) {
  return previous == 0 || static_cast<int32_t>(incoming - previous) > 0;
}

bool sequence_is_current_or_newer(const uint32_t incoming, const uint32_t previous) {
  return incoming == previous || sequence_is_newer(incoming, previous);
}

uint32_t advance_nonzero_sequence(uint32_t& sequence) {
  if (++sequence == 0)
    ++sequence;
  return sequence;
}

}  // namespace multiplayer::platform
