#pragma once

#include <cstdint>
#include <string>

#include "game/multiplayer/jak2/bridge/goal_player_types.h"

namespace multiplayer::jak2::application {
class ReplicationMailbox;
}

namespace multiplayer::jak2::bridge {

bool exchange_state(uint32_t state_address, application::ReplicationMailbox& mailbox);
bool read_string(uint32_t address, std::string& value);
bool read_appearance(uint32_t address, MPPlayerAppearance& appearance);
bool write_appearance(uint32_t address, const MPPlayerAppearance& appearance);

}  // namespace multiplayer::jak2::bridge
