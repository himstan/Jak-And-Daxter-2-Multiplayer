#pragma once

#include <cstdint>
#include <string>

#include "game/multiplayer/multiplayer_types.h"

void mp_direct_connect_clear(MultiplayerData& data);
void mp_direct_connect_reset(MultiplayerData& data);
std::string mp_direct_connect_display(const MultiplayerData& data, int field);
int mp_direct_connect_edit(MultiplayerData& data, int field, uint32_t key);
bool mp_direct_connect_ready(const MultiplayerData& data);
bool mp_direct_connect_start(MultiplayerData& data);
