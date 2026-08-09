#pragma once

#include <cstddef>
#include <cstdint>

#include "game/multiplayer/multiplayer_types.h"

bool multiplayer_valid_player_limit(uint32_t player_limit);

HostPeerSession* multiplayer_host_peer_find(MultiplayerData& data, ENetPeer* peer);
const HostPeerSession* multiplayer_host_peer_find(const MultiplayerData& data,
                                                  const ENetPeer* peer);
HostPeerSession* multiplayer_host_peer_for_player_id(MultiplayerData& data, uint32_t player_id);
HostPeerSession* multiplayer_host_peer_allocate(MultiplayerData& data,
                                                ENetPeer* peer,
                                                uint32_t current_time);
bool multiplayer_host_peer_authenticate(MultiplayerData& data,
                                        HostPeerSession& session,
                                        uint32_t current_time);
void multiplayer_host_peer_release(MultiplayerData& data, HostPeerSession& session);
void multiplayer_host_peer_release(MultiplayerData& data, ENetPeer* peer);
void multiplayer_host_peer_reset_all(MultiplayerData& data);

size_t multiplayer_host_pending_peer_count(const MultiplayerData& data);
size_t multiplayer_host_authenticated_peer_count(const MultiplayerData& data);
bool multiplayer_host_has_open_player_slot(const MultiplayerData& data);

MultiplayerSecurity* multiplayer_security_for_peer(MultiplayerData& data, ENetPeer* peer);
const MultiplayerSecurity* multiplayer_security_for_peer(const MultiplayerData& data,
                                                         const ENetPeer* peer);
uint32_t multiplayer_authenticated_sender_player_id(const MultiplayerData& data,
                                                    const ENetPeer* peer);
bool multiplayer_peer_is_authenticated(const MultiplayerData& data, const ENetPeer* peer);

void multiplayer_host_request_bootstrap_for_all(MultiplayerData& data);
