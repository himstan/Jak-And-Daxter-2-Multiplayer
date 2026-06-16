#pragma once

#include "game/multiplayer/multiplayer_types.h"

#include "enet/enet.h"

void mp_handle_palace_squid_sync_packet(MultiplayerData& data,
                                        const ENetPacket* packet,
                                        uint32_t current_time);
void mp_send_palace_squid_sync(MultiplayerData& data, MPPalaceSquidSyncBufferGOAL* buffer);
void mp_receive_palace_squid_sync(MultiplayerData& data, MPPalaceSquidSyncBufferGOAL* buffer);
