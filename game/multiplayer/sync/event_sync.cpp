#include "event_sync.h"

#include "common/log/log.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"

#include <cstring>

namespace {
constexpr uint32_t kMaxGoalEvents = 16;
constexpr size_t kMaxInboundEvents = 64;
}

void mp_handle_game_event_packet(MultiplayerData& data, const ENetPacket* packet) {
  const auto* event = PacketView(packet).as_exact<PacketGameEvent>(PacketType::EVENT_GAME);
  if (!event) {
    return;
  }

  uint32_t type = 0;
  memcpy(&type, event->raw_data, sizeof(type));
  lg::info("[Multiplayer] Received Game Event: Type {}", type);

  if (data.inbound_events.size() >= kMaxInboundEvents) {
    data.inbound_events.erase(data.inbound_events.begin());
    uint32_t now = enet_time_get();
    if (now - data.last_event_queue_debug_time > 2000) {
      lg::warn("[Multiplayer] Inbound event queue full. Dropping oldest event.");
      data.last_event_queue_debug_time = now;
    }
  }
  data.inbound_events.push_back(*event);
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
  while (!data.inbound_events.empty() && events->in_count < kMaxGoalEvents) {
    memcpy(&events->in_events[events->in_count++],
           data.inbound_events.front().raw_data,
           sizeof(MPEvent));
    data.inbound_events.erase(data.inbound_events.begin());
  }
}
