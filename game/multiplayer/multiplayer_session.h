#pragma once

#include <cstdint>

#include "enet/enet.h"
#include "game/multiplayer/multiplayer_types.h"

MultiplayerData& multiplayer_data();

bool multiplayer_debug_receive_stopped();
void multiplayer_set_debug_receive_stopped(bool stopped);

void multiplayer_reset_remote_traffic_buffers(MultiplayerData& data);
void multiplayer_reset_remote_palace_squid_state(MultiplayerData& data);
void multiplayer_reset_remote_airlock_state(MultiplayerData& data);
void multiplayer_clear_remote_peer_state(MultiplayerData& data);
void multiplayer_clear_direct_connect_draft(MultiplayerData& data);
bool multiplayer_prepare_host_for_next_peer(MultiplayerData& data,
                                             bool wait_for_reconnect = false);
bool multiplayer_begin_host_reconnect(MultiplayerData& data);
void multiplayer_clear_session_state(MultiplayerData& data,
                                      bool preserve_reconnect_state = false);
void multiplayer_request_bootstrap(MultiplayerData& data);
void multiplayer_set_status(MultiplayerData& data, int status);
void multiplayer_cleanup_stale_sync(MultiplayerData& data, uint32_t current_time);
void multiplayer_update_receive_timeout(MultiplayerData& data, uint32_t current_time);
void multiplayer_enter_client_reconnect(MultiplayerData& data, uint32_t current_time);
bool multiplayer_client_reconnect_due(const MultiplayerData& data, uint32_t current_time);
void multiplayer_note_client_reconnect_attempt_started(MultiplayerData& data);
void multiplayer_note_client_reconnect_failed(MultiplayerData& data, uint32_t current_time);
void multiplayer_note_client_reconnect_authenticated(MultiplayerData& data);
void multiplayer_note_client_reconnect_completed(MultiplayerData& data);
void multiplayer_cancel_client_reconnect(MultiplayerData& data);
void multiplayer_handle_client_handshake_timeout(MultiplayerData& data, uint32_t current_time);
bool multiplayer_handle_client_leave(MultiplayerData& data,
                                      ENetPeer* sender,
                                      MultiplayerLeaveReason reason);
bool multiplayer_handle_host_leave(MultiplayerData& data,
                                   ENetPeer* sender,
                                   MultiplayerLeaveReason reason);
