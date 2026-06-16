#pragma once

#include "game/multiplayer/multiplayer_types.h"

#include "enet/enet.h"

void mp_handle_enemy_sync_packet(MultiplayerData& data, const ENetPacket* packet, uint32_t current_time);
void mp_send_enemy_sync(MultiplayerData& data, MPEnemySyncBufferGOAL* buffer);
void mp_receive_enemy_sync(MultiplayerData& data, MPEnemySyncBufferGOAL* buffer);
