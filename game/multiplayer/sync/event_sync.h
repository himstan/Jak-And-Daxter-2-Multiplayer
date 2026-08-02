#pragma once

#include "game/multiplayer/multiplayer_types.h"

#include "enet/enet.h"

#include <cstddef>
#include <cstdint>
#include <vector>

bool mp_encode_game_event(const MPEvent& event,
                          uint32_t sequence,
                          std::vector<uint8_t>& output);
bool mp_decode_game_event(const void* data, size_t size, PacketGameEvent& output);
void mp_handle_game_event_packet(MultiplayerData& data, const ENetPacket* packet);
void mp_send_game_events(MultiplayerData& data, MPEventBufferGOAL* events);
void mp_receive_game_events(MultiplayerData& data, MPEventBufferGOAL* events);
