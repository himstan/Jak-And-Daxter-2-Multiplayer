#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "enet/enet.h"
#include "game/multiplayer/multiplayer_types.h"

bool mp_encode_game_event(const MPEvent& event, uint32_t sequence, std::vector<uint8_t>& output);
bool mp_decode_game_event(const void* data, size_t size, PacketGameEvent& output);
bool mp_handle_game_event_packet(MultiplayerData& data,
                                 const ENetPacket* packet,
                                 uint32_t sender_player_id);
void mp_send_game_events(MultiplayerData& data, MPEventBufferGOAL* events);
void mp_receive_game_events(MultiplayerData& data, MPEventBufferGOAL* events);
