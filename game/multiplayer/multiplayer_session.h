#pragma once

#include "game/multiplayer/multiplayer_types.h"

#include <cstdint>

MultiplayerData& multiplayer_data();

bool multiplayer_debug_receive_stopped();
void multiplayer_set_debug_receive_stopped(bool stopped);

void multiplayer_reset_remote_traffic_buffers(MultiplayerData& data);
void multiplayer_reset_remote_palace_squid_state(MultiplayerData& data);
void multiplayer_clear_session_state(MultiplayerData& data);
void multiplayer_request_full_sync(MultiplayerData& data);
void multiplayer_set_status(MultiplayerData& data, int status);
void multiplayer_cleanup_stale_sync(MultiplayerData& data, uint32_t current_time);
void multiplayer_update_receive_timeout(MultiplayerData& data, uint32_t current_time);
