#include "traffic_sync.h"

#include "common/log/log.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_session.h"
#include "game/multiplayer/pedestrian/multiplayer_pedestrian.h"
#include "game/multiplayer/vehicle/multiplayer_vehicle.h"

#include "game/multiplayer/multiplayer_manager.h"

namespace {
bool mp_valid_traffic_origin(uint8_t origin_player_id) {
  return origin_player_id < kMPMaxPlayers;
}

bool mp_valid_ambient_sequence(uint32_t sequence, uint32_t first_sequence) {
  return sequence >= first_sequence && sequence < 65535u;
}
}  // namespace

size_t mp_traffic_packet_size(uint32_t count, size_t element_size) {
  constexpr size_t prefix_size = sizeof(PacketHeader) + sizeof(uint8_t) + 3 + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t);
  return prefix_size + (element_size * count);
}

uint32_t mp_make_traffic_net_id(uint32_t entity_class,
                                uint8_t origin_player_id,
                                uint32_t sequence) {
  const bool valid_class = entity_class == kMPTrafficPedestrianNetIdClass ||
                           entity_class == kMPTrafficVehicleNetIdClass ||
                           entity_class == kMPPlayerVehicleNetIdClass;
  if (!valid_class || !mp_valid_traffic_origin(origin_player_id) ||
      sequence > kMPTrafficNetIdSequenceMask) {
    return 0;
  }
  return entity_class | (static_cast<uint32_t>(origin_player_id) << kMPTrafficNetIdOriginShift) |
         sequence;
}

uint32_t mp_traffic_net_id_class(uint32_t net_id) {
  return net_id & kMPTrafficNetIdClassMask;
}

uint8_t mp_traffic_net_id_origin(uint32_t net_id) {
  return static_cast<uint8_t>((net_id & kMPTrafficNetIdOriginMask) >>
                              kMPTrafficNetIdOriginShift);
}

uint32_t mp_traffic_net_id_sequence(uint32_t net_id) {
  return net_id & kMPTrafficNetIdSequenceMask;
}

bool mp_validate_pedestrian_net_id(uint32_t net_id, uint8_t source_player_id) {
  const uint32_t entity_class = mp_traffic_net_id_class(net_id);
  if (entity_class == kMPFixedTrafficNetIdClass) {
    return (net_id & kMPFixedTrafficNetIdNamespaceMask) ==
           kMPShuttlePedestrianNetIdNamespace;
  }
  return entity_class == kMPTrafficPedestrianNetIdClass &&
         mp_valid_traffic_origin(source_player_id) &&
         mp_traffic_net_id_origin(net_id) == source_player_id &&
         mp_valid_ambient_sequence(mp_traffic_net_id_sequence(net_id), 1u);
}

bool mp_validate_vehicle_net_id(uint32_t net_id) {
  const uint32_t entity_class = mp_traffic_net_id_class(net_id);
  if (entity_class == kMPFixedTrafficNetIdClass) {
    return (net_id & kMPFixedTrafficNetIdNamespaceMask) ==
               kMPMissionVehicleNetIdNamespace &&
           net_id != kMPMissionVehicleNetIdNamespace;
  }
  if (!mp_valid_traffic_origin(mp_traffic_net_id_origin(net_id))) {
    return false;
  }
  if (entity_class == kMPTrafficVehicleNetIdClass) {
    return mp_valid_ambient_sequence(mp_traffic_net_id_sequence(net_id), 32768u);
  }
  return entity_class == kMPPlayerVehicleNetIdClass;
}

uint8_t mp_traffic_authority_for_player(const MultiplayerData& data, uint32_t player_id) {
  if (player_id >= kMPMaxPlayers) {
    return kMPInvalidCompactPlayerId;
  }
  return data.traffic_authority_map[player_id];
}

bool mp_validate_traffic_source(const MultiplayerData& data,
                                uint8_t source_player_id,
                                uint32_t sender_player_id,
                                uint32_t authority_revision) {
  if (source_player_id >= kMPMaxPlayers ||
      mp_traffic_authority_for_player(data, source_player_id) != source_player_id) {
    return false;
  }
  if (data.traffic_authority_revision == 0 ||
      authority_revision != data.traffic_authority_revision) {
    return false;
  }
  if (data.session_role == 0) {
    return sender_player_id == source_player_id;
  }
  if (data.session_role == 1) {
    return sender_player_id == data.host_player_id &&
           data.selected_traffic_authority == source_player_id;
  }
  return false;
}

void mp_send_traffic_authority(MultiplayerData& data, ENetPeer* peer) {
  if (data.session_role != 0 || !data.host) {
    return;
  }
  PacketTrafficAuthority packet = {};
  packet.header.type = PacketType::TRAFFIC_AUTHORITY;
  packet.header.sequenceNum = ++data.sequence_num;
  packet.revision = data.traffic_authority_revision;
  memcpy(packet.assignments, data.traffic_authority_map.data(), kMPMaxPlayers);

  if (peer) {
    mp_send_packet_immediately(data, peer, static_cast<int>(MultiplayerChannel::CONTROL), &packet,
                               sizeof(packet), ENET_PACKET_FLAG_RELIABLE);
  } else {
    MultiplayerManager::broadcast(data, static_cast<int>(MultiplayerChannel::CONTROL), packet,
                                  ENET_PACKET_FLAG_RELIABLE);
  }
}

uint32_t mp_set_traffic_authority_map(MultiplayerData& data,
                                      const uint8_t* authority_map) {
  if (data.session_role != 0 || !authority_map) {
    return data.traffic_authority_revision;
  }

  std::array<uint8_t, kMPMaxPlayers> normalized_map = mp_invalid_traffic_authority_map();

  for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    const uint8_t source = authority_map[player_id];
    if (source < kMPMaxPlayers) {
      normalized_map[player_id] = source;
    }
  }

  for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    const uint8_t source = normalized_map[player_id];
    const uint8_t source_authority =
        source < kMPMaxPlayers ? normalized_map[source] : kMPInvalidCompactPlayerId;
    if (source < kMPMaxPlayers && source_authority != source) {
      normalized_map[player_id] = kMPInvalidCompactPlayerId;
    }
  }

  const bool map_changed = (data.traffic_authority_map != normalized_map);
  if (!map_changed) {
    return data.traffic_authority_revision;
  }

  data.traffic_authority_map = normalized_map;
  ++data.traffic_authority_revision;
  if (data.traffic_authority_revision == 0) {
    ++data.traffic_authority_revision;
  }

  if (data.session_role == 0 && data.host) {
    mp_send_traffic_authority(data, nullptr);
  }
  return data.traffic_authority_revision;
}

void mp_set_selected_traffic_authority(MultiplayerData& data, uint32_t selected_authority) {
  const uint8_t requested_source =
      selected_authority < kMPMaxPlayers ? static_cast<uint8_t>(selected_authority)
                                         : kMPInvalidCompactPlayerId;
  const uint8_t new_source =
      requested_source < kMPMaxPlayers &&
              mp_traffic_authority_for_player(data, requested_source) == requested_source
          ? requested_source
          : kMPInvalidCompactPlayerId;
  const uint8_t old_source = data.selected_traffic_authority;
  data.selected_traffic_authority = new_source;
  if (old_source != new_source) {
    multiplayer_reset_remote_traffic_buffers(data);
  }
}

bool mp_handle_traffic_authority_packet(MultiplayerData& data,
                                        const _ENetPacket* packet,
                                        uint32_t sender_player_id) {
  if (data.session_role != 1 || sender_player_id != data.host_player_id) {
    return false;
  }
  const auto auth = PacketView(packet).as_exact<PacketTrafficAuthority>(PacketType::TRAFFIC_AUTHORITY);
  if (!auth || auth->revision == 0) {
    return false;
  }
  if (auth->revision < data.traffic_authority_revision) {
    return false;
  }

  std::array<uint8_t, kMPMaxPlayers> normalized_map = mp_invalid_traffic_authority_map();
  for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    const uint8_t source = auth->assignments[player_id];
    if (source < kMPMaxPlayers) {
      normalized_map[player_id] = source;
    }
  }
  for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    const uint8_t source = normalized_map[player_id];
    const uint8_t source_authority =
        source < kMPMaxPlayers ? normalized_map[source] : kMPInvalidCompactPlayerId;
    if (source < kMPMaxPlayers && source_authority != source) {
      normalized_map[player_id] = kMPInvalidCompactPlayerId;
    }
  }

  if (auth->revision == data.traffic_authority_revision) {
    return normalized_map == data.traffic_authority_map;
  }

  data.traffic_authority_map = normalized_map;
  data.traffic_authority_revision = auth->revision;
  data.selected_traffic_authority = kMPInvalidCompactPlayerId;
  multiplayer_reset_remote_traffic_buffers(data);
  return true;
}

bool mp_accept_traffic_level(MultiplayerData& data,
                             uint32_t level_hash,
                             uint32_t,
                             const char* label,
                             uint32_t) {
  if (level_hash != 0 && data.remote_traffic_buffer_level_hash != 0 &&
      level_hash != data.remote_traffic_buffer_level_hash) {
    lg::info("[Multiplayer] Traffic primary context changed for {}. old={} new={}; preserving per-entry state",
             label, data.remote_traffic_buffer_level_hash, level_hash);
  }
  if (level_hash != 0) {
    data.remote_traffic_buffer_level_hash = level_hash;
  }
  return true;
}

void mp_send_traffic_sync(MultiplayerData& data, MPTrafficSyncBufferGOAL* buffer) {
  if (!buffer || data.local_player_id >= kMPMaxPlayers ||
      mp_traffic_authority_for_player(data, data.local_player_id) != data.local_player_id) {
    return;
  }
  const int channel = static_cast<int>(MultiplayerChannel::STATE);
  send_pedestrian_sync_packets(data, buffer, channel);
  send_vehicle_sync_packets(data, buffer, channel);
}

void mp_receive_traffic_sync(MultiplayerData& data, MPTrafficSyncBufferGOAL* buffer) {
  if (!buffer) {
    return;
  }
  receive_pedestrian_sync_data(data, buffer);
  receive_vehicle_sync_data(data, buffer);
}
