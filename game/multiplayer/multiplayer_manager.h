#pragma once

#include <array>
#include <optional>

#include "multiplayer_protocol.h"
#include "multiplayer_types.h"

#include "enet/enet.h"

bool multiplayer_valid_player_character_config(
    const std::array<MPPlayerCharacter, kMPMaxPlayers>& player_characters);

class MultiplayerManager {
 public:
  static void setup_host(
      MultiplayerData& data,
      bool internet_host,
      uint32_t player_limit,
      const std::array<MPPlayerCharacter, kMPMaxPlayers>& player_characters);
  static void setup_client(MultiplayerData& data, const char* ip, int port);
  static void disconnect(MultiplayerData& data, bool preserve_reconnect_state = false);

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
  static bool broadcast_except(MultiplayerData& data,
                               ENetPeer* excluded_peer,
                               int channel,
                               const void* packet_data,
                               size_t size,
                               ENetPacketFlag flags,
                               std::optional<uint32_t> stream_key_override = std::nullopt);
  static bool send_to_peer(MultiplayerData& data,
                           ENetPeer* peer,
                           int channel,
                           const void* packet_data,
                           size_t size,
                           ENetPacketFlag flags);

  template <typename T>
  static bool send_to_peer(MultiplayerData& data,
                           ENetPeer* peer,
                           int channel,
                           const T& packet_data,
                           ENetPacketFlag flags) {
    return send_to_peer(data, peer, channel, &packet_data, sizeof(T), flags);
  }

 private:
  static void discovery_responder_func(MultiplayerData* data);
};

MultiplayerHostCopyMode multiplayer_host_copy_mode(MultiplayerData& data);
