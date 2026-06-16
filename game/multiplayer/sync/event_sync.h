#pragma once

#include "game/multiplayer/multiplayer_types.h"

#include "enet/enet.h"

void mp_handle_game_event_packet(MultiplayerData& data, const ENetPacket* packet);
void mp_send_game_events(MultiplayerData& data, MPEventBufferGOAL* events);
void mp_receive_game_events(MultiplayerData& data, MPEventBufferGOAL* events);
