#include "game/multiplayer/jak2/wire/packets/game_event_batch_packet.h"

#include "game/multiplayer/jak2/wire/event_types.h"

namespace multiplayer::jak2::wire {

bool valid_event(const GameEventRecord& event) {
  const auto* descriptor = event_descriptor(event.event_id);
  return descriptor && event.payload_size == descriptor->payload_size &&
         event.payload_size <= event.payload.size();
}

bool validate_packet(const GameEventBatchPacket& packet) {
  if (packet.events.empty() || packet.events.size() > UINT8_MAX) {
    return false;
  }
  for (const auto& event : packet.events) {
    if (!valid_event(event)) {
      return false;
    }
  }
  return true;
}

GameEventBatchPacket to_packet(const core::GameEventBatch& batch) {
  GameEventBatchPacket packet;
  packet.events.reserve(batch.events.size());
  for (const auto& event : batch.events) {
    packet.events.push_back(
        {.event_id = event.event_id, .payload_size = event.payload_size, .payload = event.payload});
  }
  return packet;
}

void from_packet(const GameEventBatchPacket& packet, core::GameEventBatch& batch) {
  batch = {};
  batch.events.reserve(packet.events.size());
  for (const auto& [event_id, payload_size, payload] : packet.events) {
    batch.events.push_back(
        {.event_id = event_id, .payload_size = payload_size, .payload = payload});
  }
}

}  // namespace multiplayer::jak2::wire
