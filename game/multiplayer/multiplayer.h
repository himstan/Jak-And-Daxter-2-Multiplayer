#pragma once
#include "common/common_types.h"

void init_multiplayer_pc_port();
void pc_multi_disconnect();

void pc_multi_setup_host();
void pc_multi_setup_client(u32 ip_ptr, u32 port);
int64_t pc_multi_get_status();
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
int pc_multi_get_ping_valid();
float pc_multi_get_packet_loss_percent();
float pc_multi_get_packet_loss_variance_percent();
int pc_multi_get_ping_variance();
int pc_multi_get_send_packet_rate();
int pc_multi_get_recv_packet_rate();
u64 pc_multi_get_wire_total_sent_bytes();
u64 pc_multi_get_wire_total_received_bytes();
u64 pc_multi_get_wire_total_sent_packets();
u64 pc_multi_get_wire_total_received_packets();
void pc_multi_send_palace_squid(u32 buffer_ptr);
void pc_multi_receive_palace_squid(u32 buffer_ptr);
void pc_multi_send_airlock_state(u32 buffer_ptr);
void pc_multi_receive_airlock_state(u32 buffer_ptr);
