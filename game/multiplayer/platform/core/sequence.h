#pragma once

#include <cstdint>

namespace multiplayer::platform {

bool sequence_is_newer(uint32_t incoming, uint32_t previous);
bool sequence_is_current_or_newer(uint32_t incoming, uint32_t previous);
uint32_t advance_nonzero_sequence(uint32_t& sequence);

}  // namespace multiplayer::platform
