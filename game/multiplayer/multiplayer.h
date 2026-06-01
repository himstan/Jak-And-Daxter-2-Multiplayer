#pragma once
#include "common/common_types.h"

void init_multiplayer_pc_port();
void pc_multi_disconnect();

void pc_multi_setup_host();
void pc_multi_setup_client(u32 ip_ptr, u32 port);
int pc_multi_get_status();
void pc_multi_request_full_sync();
void pc_multi_stop_search();
void pc_multi_start_search();
u64 pc_multi_get_command_line_arg(u32 str_ptr);

// Granular Sync Functions
int pc_multi_get_role();
void pc_multi_poll(u32 local_ptr, u32 remote_ptr);
void pc_multi_send_state(u32 local_ptr);
void pc_multi_receive_state(u32 remote_ptr);
void pc_multi_send_events(u32 event_ptr);
void pc_multi_receive_events(u32 event_ptr);
u64 pc_multi_get_ticks();
int pc_multi_get_ping();
int pc_multi_get_packet_loss();
void pc_multi_send_palace_squid(u32 buffer_ptr);
void pc_multi_receive_palace_squid(u32 buffer_ptr);
