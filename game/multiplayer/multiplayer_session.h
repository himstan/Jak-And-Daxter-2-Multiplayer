#pragma once

#include <cstdint>

#include "game/multiplayer/multiplayer_types.h"

MultiplayerData& multiplayer_data();

bool multiplayer_debug_receive_stopped();
void multiplayer_set_debug_receive_stopped(bool stopped);

void multiplayer_reset_remote_traffic_buffers(MultiplayerData& data);
void multiplayer_reset_remote_palace_squid_state(MultiplayerData& data);
void multiplayer_reset_remote_airlock_state(MultiplayerData& data);
void multiplayer_clear_remote_peer_state(MultiplayerData& data);
void multiplayer_clear_direct_connect_draft(MultiplayerData& data);
bool multiplayer_prepare_host_for_next_peer(MultiplayerData& data);
void multiplayer_clear_session_state(MultiplayerData& data);
void multiplayer_request_full_sync(MultiplayerData& data);
void multiplayer_set_status(MultiplayerData& data, int status);
void multiplayer_cleanup_stale_sync(MultiplayerData& data, uint32_t current_time);
void multiplayer_update_receive_timeout(MultiplayerData& data, uint32_t current_time);
