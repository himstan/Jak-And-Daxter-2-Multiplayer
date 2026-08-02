#pragma once

#include "multiplayer_types.h"
#include "multiplayer_protocol.h"
#include "enet/enet.h"

class MultiplayerManager {
 public:
  static void setup_host(MultiplayerData& data, bool internet_host);
  static void setup_client(MultiplayerData& data, const char* ip, int port);
  static void disconnect(MultiplayerData& data, bool preserve_reconnect_state = false);
  static bool retry_online_setup(MultiplayerData& data);

  static bool broadcast(MultiplayerData& data,
                        int channel,
                        const void* packet_data,
                        size_t size,
                        ENetPacketFlag flags);

  template <typename T>
  static bool broadcast(MultiplayerData& data,
                        int channel,
                        const T& packet_data,
                        ENetPacketFlag flags) {
    return broadcast(data, channel, &packet_data, sizeof(T), flags);
  }
  static void send_to_peer(ENetPeer* peer,
                           int channel,
                           const void* packet_data,
                           size_t size,
                           ENetPacketFlag flags);

 private:
  static void discovery_responder_func(MultiplayerData* data);
};

int multiplayer_host_invite_status(MultiplayerData& data);
