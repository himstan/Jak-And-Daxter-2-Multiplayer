#pragma once

#include "enet/enet.h"
#include "game/multiplayer/multiplayer_types.h"

bool mp_valid_player_id(uint32_t player_id);
bool mp_player_id_allowed_from_sender(const MultiplayerData& data,
                                      uint32_t sender_player_id,
                                      uint32_t claimed_player_id);
void mp_clear_player_slot(MultiplayerData& data,
                          MPPlayerControllerGOAL* controller,
                          uint32_t player_id);

bool mp_handle_player_state_packet(MultiplayerData& data,
                                   const ENetPacket* packet,
                                   uint32_t sender_player_id,
                                   uint32_t current_time);
bool mp_handle_turret_state_packet(MultiplayerData& data,
                                   const ENetPacket* packet,
                                   uint32_t sender_player_id);
bool mp_handle_join_packet(MultiplayerData& data,
                           const ENetPacket* packet,
                           uint32_t sender_player_id,
                           MPPlayerControllerGOAL* controller);
void mp_handle_world_state_packet(MultiplayerData& data,
                                  const ENetPacket* packet,
                                  MPWorldSyncStateGOAL* world);
void mp_handle_bootstrap_packet(const ENetPacket* packet,
                                MPWorldSyncStateGOAL* world,
                                MPBootstrapSyncStateGOAL* bootstrap);
void mp_seed_peer_roster(MultiplayerData& data,
                         ENetPeer* peer,
                         const MPPlayerControllerGOAL* controller);

void mp_send_player_sync(MultiplayerData& data,
                         MPPlayerControllerGOAL* controller,
                         MPWorldSyncStateGOAL* world,
                         MPBootstrapSyncStateGOAL* bootstrap);
void mp_receive_player_sync(MultiplayerData& data,
                            MPPlayerControllerGOAL* controller,
                            MPWorldSyncStateGOAL* world,
                            MPBootstrapSyncStateGOAL* bootstrap);
