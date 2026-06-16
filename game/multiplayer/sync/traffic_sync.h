#pragma once

#include "game/multiplayer/multiplayer_types.h"

#include <cstddef>
#include <cstdint>

size_t mp_traffic_packet_size(uint32_t count, size_t element_size);
bool mp_accept_traffic_level(MultiplayerData& data,
                             uint32_t level_hash,
                             uint32_t count,
                             const char* label,
                             uint32_t current_time);

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
