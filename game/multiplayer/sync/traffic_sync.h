#pragma once

#include "game/multiplayer/multiplayer_types.h"

#include <cstddef>
#include <cstdint>

inline constexpr uint32_t kMPTrafficNetIdClassMask = 0xf0000000u;
inline constexpr uint32_t kMPTrafficNetIdOriginMask = 0x0f000000u;
inline constexpr uint32_t kMPTrafficNetIdSequenceMask = 0x00ffffffu;
inline constexpr uint32_t kMPTrafficNetIdOriginShift = 24u;
inline constexpr uint32_t kMPTrafficPedestrianNetIdClass = 0x10000000u;
inline constexpr uint32_t kMPTrafficVehicleNetIdClass = 0x20000000u;
inline constexpr uint32_t kMPPlayerVehicleNetIdClass = 0x40000000u;
inline constexpr uint32_t kMPFixedTrafficNetIdClass = 0x70000000u;
inline constexpr uint32_t kMPFixedTrafficNetIdNamespaceMask = 0xff000000u;
inline constexpr uint32_t kMPMissionVehicleNetIdNamespace = 0x70000000u;
inline constexpr uint32_t kMPShuttlePedestrianNetIdNamespace = 0x71000000u;

size_t mp_traffic_packet_size(uint32_t count, size_t element_size);
uint32_t mp_make_traffic_net_id(uint32_t entity_class,
                                uint8_t origin_player_id,
                                uint32_t sequence);
uint32_t mp_traffic_net_id_class(uint32_t net_id);
uint8_t mp_traffic_net_id_origin(uint32_t net_id);
uint32_t mp_traffic_net_id_sequence(uint32_t net_id);
bool mp_validate_pedestrian_net_id(uint32_t net_id, uint8_t source_player_id);
bool mp_validate_vehicle_net_id(uint32_t net_id);
bool mp_accept_traffic_level(MultiplayerData& data,
                             uint32_t level_hash,
                             uint32_t count,
                             const char* label,
                             uint32_t current_time);
uint8_t mp_traffic_authority_for_player(const MultiplayerData& data, uint32_t player_id);
bool mp_validate_traffic_source(const MultiplayerData& data,
                                uint8_t source_player_id,
                                uint32_t sender_player_id,
                                uint32_t authority_revision);
void mp_send_traffic_authority(MultiplayerData& data, struct _ENetPeer* peer = nullptr);
uint32_t mp_set_traffic_authority_map(MultiplayerData& data,
                                      const uint8_t* authority_map);
void mp_set_selected_traffic_authority(MultiplayerData& data,
                                       uint32_t selected_authority);
bool mp_handle_traffic_authority_packet(MultiplayerData& data,
                                        const struct _ENetPacket* packet,
                                        uint32_t sender_player_id);

template <typename T, typename IdGetter>
T* mp_find_matching_or_empty_slot(T* items, uint32_t count, uint32_t id, IdGetter id_getter) {
  T* empty_slot = nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t item_id = id_getter(items[i]);
    if (item_id == id) {
      return &items[i];
    }
    if (!empty_slot && item_id == 0) {
      empty_slot = &items[i];
    }
  }
  return empty_slot;
}

void mp_send_traffic_sync(MultiplayerData& data, MPTrafficSyncBufferGOAL* buffer);
void mp_receive_traffic_sync(MultiplayerData& data, MPTrafficSyncBufferGOAL* buffer);
