#pragma once

#include <cstdint>
#include <string>

#include "game/multiplayer/jak2/bridge/goal_player_types.h"

struct MultiplayerPreferences;

namespace multiplayer::jak2::application {
class ReplicationMailbox;
}

namespace multiplayer::jak2::bridge {

bool exchange_state(uint32_t state_address, application::ReplicationMailbox& mailbox);
bool read_string(uint32_t address, std::string& value);
bool read_preferences(uint32_t address, MultiplayerPreferences& preferences);
bool write_preferences(uint32_t address, const MultiplayerPreferences& preferences);
bool read_skin(uint32_t address, MPPlayerSkin& skin);

}  // namespace multiplayer::jak2::bridge
