#pragma once

#include "game/multiplayer/multiplayer_types.h"
#include "game/kernel/common/kscheme.h"
#include "enet/enet.h"

bool handle_vehicle_sync_packet(const _ENetEvent& event,
                                MultiplayerData& data,
                                uint32_t sender_player_id);
void send_vehicle_sync_packets(MultiplayerData& data, MPTrafficSyncBufferGOAL* buffer, int exclude_peer);
void receive_vehicle_sync_data(MultiplayerData& data, MPTrafficSyncBufferGOAL* buffer);
