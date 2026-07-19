#include "event_sync.h"

#include "common/log/log.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"

#include <cstring>

namespace {
constexpr uint32_t kMaxGoalEvents = 16;
constexpr size_t kMaxInboundEvents = 64;

bool known_event_type(uint32_t type) {
  switch (type) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 6:
    case 7:
    case 8:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
      return true;
    default:
      return false;
  }
}
}

void mp_handle_game_event_packet(MultiplayerData& data, const ENetPacket* packet) {
  const auto event = PacketView(packet).as_exact<PacketGameEvent>(PacketType::EVENT_GAME);
  if (!event) {
    return;
  }

  uint32_t type = 0;
  memcpy(&type, event->raw_data, sizeof(type));
  if (!known_event_type(type)) {
    return;
  }
  const uint32_t now = enet_time_get();
  if (now - data.last_event_receive_debug_time > 2000) {
    lg::info("[Multiplayer] Receiving game events. Latest type {}", type);
    data.last_event_receive_debug_time = now;
  }

  if (data.inbound_events.size() >= kMaxInboundEvents) {
    if (now - data.last_event_queue_debug_time > 2000) {
      lg::warn("[Multiplayer] Inbound event queue full. Dropping oldest event.");
      data.last_event_queue_debug_time = now;
    }
  }
  data.inbound_events.push_overwrite(*event);
}

void mp_send_game_events(MultiplayerData& data, MPEventBufferGOAL* events) {
  if (!events || events->out_count == 0) {
    return;
  }

  uint32_t out_count = mp_clamp_count(events->out_count, kMaxGoalEvents);
  for (uint32_t i = 0; i < out_count; ++i) {
    PacketGameEvent out_event = {};
    out_event.header.type = PacketType::EVENT_GAME;
    out_event.header.sequenceNum = ++data.last_out_event_seq;
    memcpy(out_event.raw_data, &events->out_events[i], sizeof(MPEvent));
    MultiplayerManager::broadcast(data, data.local_role, out_event, ENET_PACKET_FLAG_RELIABLE);
  }
  events->out_count = 0;
}

void mp_receive_game_events(MultiplayerData& data, MPEventBufferGOAL* events) {
  if (!events) {
    return;
  }

  if (events->in_count > kMaxGoalEvents) {
    events->in_count = kMaxGoalEvents;
  }
  if (!data.inbound_events.empty()) {
    lg::info("[Multiplayer] Moving {} events to GOAL. Current in_count: {}",
             data.inbound_events.size(), events->in_count);
  }
  PacketGameEvent incoming = {};
  while (events->in_count < kMaxGoalEvents && data.inbound_events.pop(incoming)) {
    memcpy(&events->in_events[events->in_count++], incoming.raw_data, sizeof(MPEvent));
  }
}
