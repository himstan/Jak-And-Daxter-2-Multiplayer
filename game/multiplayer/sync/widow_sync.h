#pragma once

#include "game/multiplayer/multiplayer_types.h"

#include "enet/enet.h"

void mp_handle_widow_sync_packet(MultiplayerData& data,
                                 const ENetPacket* packet,
                                 uint32_t current_time);
void mp_send_widow_sync(MultiplayerData& data, MPWidowSyncBufferGOAL* buffer);
void mp_receive_widow_sync(MultiplayerData& data, MPWidowSyncBufferGOAL* buffer);
