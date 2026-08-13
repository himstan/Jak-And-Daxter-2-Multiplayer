#include "traffic_sync.h"

#include "common/log/log.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_session.h"
#include "game/multiplayer/pedestrian/multiplayer_pedestrian.h"
#include "game/multiplayer/vehicle/multiplayer_vehicle.h"

namespace {
bool mp_valid_traffic_origin(uint8_t origin_player_id) {
  return origin_player_id < kMPMaxPlayers;
}

bool mp_valid_ambient_sequence(uint32_t sequence, uint32_t first_sequence) {
  return sequence >= first_sequence && sequence < 65535u;
}
}  // namespace

size_t mp_traffic_packet_size(uint32_t count, size_t element_size) {
  constexpr size_t source_prefix_size = sizeof(uint8_t) + 3;
  return sizeof(PacketHeader) + source_prefix_size + sizeof(uint32_t) + sizeof(uint64_t) +
         sizeof(uint32_t) + (element_size * count);
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
  return static_cast<uint8_t>((data.traffic_authority_map >> (player_id * 8u)) & 0xffu);
}

bool mp_validate_traffic_source(const MultiplayerData& data,
                                uint8_t source_player_id,
                                uint32_t sender_player_id) {
  if (source_player_id >= kMPMaxPlayers ||
      mp_traffic_authority_for_player(data, source_player_id) != source_player_id) {
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

void mp_set_traffic_authority_map(MultiplayerData& data,
                                  uint32_t authority_map,
                                  uint32_t selected_authority) {
  uint32_t normalized_map = 0xffffffffu;
  for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    const uint8_t source = static_cast<uint8_t>((authority_map >> (player_id * 8u)) & 0xffu);
    if (source < kMPMaxPlayers) {
      normalized_map &= ~(0xffu << (player_id * 8u));
      normalized_map |= static_cast<uint32_t>(source) << (player_id * 8u);
    }
  }
  for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    const uint8_t source = static_cast<uint8_t>((normalized_map >> (player_id * 8u)) & 0xffu);
    const uint8_t source_authority =
        source < kMPMaxPlayers
            ? static_cast<uint8_t>((normalized_map >> (source * 8u)) & 0xffu)
            : kMPInvalidCompactPlayerId;
    if (source < kMPMaxPlayers && source_authority != source) {
      normalized_map |= 0xffu << (player_id * 8u);
    }
  }

  const uint8_t old_source = data.selected_traffic_authority;
  data.traffic_authority_map = normalized_map;
  const uint8_t requested_source =
      selected_authority < kMPMaxPlayers ? static_cast<uint8_t>(selected_authority)
                                         : kMPInvalidCompactPlayerId;
  const uint8_t new_source =
      requested_source < kMPMaxPlayers &&
              mp_traffic_authority_for_player(data, requested_source) == requested_source
          ? requested_source
          : kMPInvalidCompactPlayerId;
  data.selected_traffic_authority = new_source;
  if (old_source != new_source) {
    multiplayer_reset_remote_traffic_buffers(data);
  }
}

bool mp_accept_traffic_level(MultiplayerData& data,
                             uint32_t level_hash,
                             uint32_t count,
                             const char* label,
                             uint32_t current_time) {
  if (level_hash != 0 && data.last_remote_traffic_level_hash != 0 &&
      level_hash != data.last_remote_traffic_level_hash) {
    if (current_time - data.last_traffic_drop_debug_time > 1000) {
      lg::info("[Multiplayer] Dropped {} traffic for level mismatch. packetLevel={} remoteLevel={} count={}",
               label, level_hash, data.last_remote_traffic_level_hash, count);
      data.last_traffic_drop_debug_time = current_time;
    }
    return false;
  }

  if (level_hash != 0 && data.remote_traffic_buffer_level_hash != 0 &&
      level_hash != data.remote_traffic_buffer_level_hash) {
    uint32_t old_level = data.remote_traffic_buffer_level_hash;
    multiplayer_reset_remote_traffic_buffers(data);
    lg::info("[Multiplayer] Reset remote traffic table for {} level change. old={} new={}",
             label, old_level, level_hash);
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
  bool has_follower = false;
  for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    if (player_id != data.local_player_id &&
        mp_traffic_authority_for_player(data, player_id) == data.local_player_id) {
      has_follower = true;
      break;
    }
  }
  if (!has_follower) {
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
