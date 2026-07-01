#pragma once

#include "game/multiplayer/multiplayer_types.h"

#include "enet/enet.h"

void mp_handle_airlock_sync_packet(MultiplayerData& data,
                                   const ENetPacket* packet,
                                   uint32_t current_time);
void mp_send_airlock_sync(MultiplayerData& data, MPAirlockSyncBufferGOAL* buffer);
void mp_receive_airlock_sync(MultiplayerData& data, MPAirlockSyncBufferGOAL* buffer);
