#pragma once

#include "game/multiplayer/multiplayer_types.h"

#include "enet/enet.h"

#include <cstdint>

void mp_handle_player_state_packet(MultiplayerData& data,
                                   const ENetPacket* packet,
                                   RemotePlayerInfoGOAL* remote,
                                   uint32_t current_time);
void mp_handle_turret_state_packet(MultiplayerData& data, const ENetPacket* packet);
void mp_handle_full_sync_packet(const ENetPacket* packet,
                                LocalPlayerInfoGOAL* local,
                                RemotePlayerInfoGOAL* remote);
void mp_send_player_state(MultiplayerData& data, LocalPlayerInfoGOAL* local);
void mp_sync_remote_player_to_goal(MultiplayerData& data, RemotePlayerInfoGOAL* remote_goal);
