#include "event_sync.h"

#include "common/log/log.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_wire_codec.h"
#include "game/multiplayer/sync/player_sync.h"

#include <cstring>
#include <vector>

namespace {
constexpr uint32_t kMaxGoalEvents = 16;
constexpr size_t kMaxEventPayload = 480;

bool decode_game_event_bytes(const void* data, size_t size, PacketGameEvent& output) {
  if (!data || size < kEventEnvelopeHeaderWireSize) {
    return false;
  }

  const auto* bytes = static_cast<const uint8_t*>(data);
  multiplayer::wire::Reader reader(bytes, size);
  uint8_t type = 0;
  uint32_t sequence = 0;
  uint32_t source_player_id = kMPInvalidPlayerId;
  uint32_t event_id = 0;
  uint16_t payload_size = 0;
  if (!reader.read_u8(type) || type != static_cast<uint8_t>(PacketType::EVENT_GAME) ||
      !reader.read_u32(sequence) || !reader.read_u32(source_player_id) ||
      !mp_valid_player_id(source_player_id) || !reader.read_u32(event_id) ||
      !multiplayer::schema::event_descriptor(event_id) ||
      !reader.read_u16(payload_size) || payload_size > kMaxEventPayload ||
      reader.remaining() != payload_size) {
    return false;
  }

  const uint8_t* payload = reader.read_span(payload_size);
  if (!payload || !reader.consumed_all()) {
    return false;
  }

  output = {};
  output.header.type = static_cast<PacketType>(type);
  output.header.sequenceNum = sequence;
  output.source_player_id = source_player_id;
  output.event_id = event_id;
  output.payload_size = payload_size;
  memcpy(output.payload, payload, payload_size);
  return true;
}

bool encode_game_event_bytes(const MPEvent& event,
                             uint32_t sequence,
                             std::vector<uint8_t>& output) {
  const uint32_t payload_size = event.payload_size == 0 ? kMaxEventPayload : event.payload_size;
  if (payload_size > kMaxEventPayload || !mp_valid_player_id(event.source_player_id) ||
      !multiplayer::schema::event_descriptor(event.etype)) {
    return false;
  }

  output.resize(kEventEnvelopeHeaderWireSize + payload_size);
  multiplayer::wire::Writer writer(output.data(), output.size());
  if (!writer.write_u8(static_cast<uint8_t>(PacketType::EVENT_GAME)) ||
      !writer.write_u32(sequence) || !writer.write_u32(event.source_player_id) ||
      !writer.write_u32(event.etype) ||
      !writer.write_u16(static_cast<uint16_t>(payload_size)) ||
      !writer.write_bytes(event.data, payload_size)) {
    return false;
  }
  output.resize(writer.size());
  return true;
}
}  // namespace

bool mp_encode_game_event(const MPEvent& event,
                          uint32_t sequence,
                          std::vector<uint8_t>& output) {
  return encode_game_event_bytes(event, sequence, output);
}

bool mp_decode_game_event(const void* data, size_t size, PacketGameEvent& output) {
  return decode_game_event_bytes(data, size, output);
}

void mp_handle_game_event_packet(MultiplayerData& data, const ENetPacket* packet) {
  PacketView view(packet);
  PacketGameEvent event = {};
  if (!view.has_header() || view.type() != PacketType::EVENT_GAME ||
      !mp_decode_game_event(view.data(), view.size(), event) ||
      !mp_player_id_matches_authenticated_peer(data, event.source_player_id)) {
    return;
  }

  const uint32_t now = enet_time_get();
  if (now - data.last_event_receive_debug_time > 2000) {
    lg::info("[Multiplayer] Receiving game event {} ({} bytes)",
             event.event_id,
             event.payload_size);
    data.last_event_receive_debug_time = now;
  }

  if (!data.inbound_events.try_push(event)) {
    if (now - data.last_event_queue_debug_time > 2000) {
      lg::warn("[Multiplayer] Inbound event queue full. Dropping newest event.");
      data.last_event_queue_debug_time = now;
    }
  }
}

void mp_send_game_events(MultiplayerData& data, MPEventBufferGOAL* events) {
  if (!events || events->out_count == 0) {
    return;
  }

  const uint32_t out_count = mp_clamp_count(events->out_count, kMaxGoalEvents);
  for (uint32_t i = 0; i < out_count; ++i) {
    events->out_events[i].source_player_id = data.local_player_id;
    std::vector<uint8_t> encoded;
    if (!mp_encode_game_event(events->out_events[i], ++data.last_out_event_seq, encoded)) {
      lg::warn("[Multiplayer] Dropping oversized event {}.", events->out_events[i].etype);
      continue;
    }
    if (MultiplayerManager::broadcast(data,
                                      data.session_role,
                                      encoded.data(),
                                      encoded.size(),
                                      ENET_PACKET_FLAG_RELIABLE)) {
      lg::debug("[Multiplayer] Submitted event id={}", events->out_events[i].etype);
    }
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
  PacketGameEvent incoming = {};
  while (events->in_count < kMaxGoalEvents && data.inbound_events.pop(incoming)) {
    MPEvent& goal_event = events->in_events[events->in_count++];
    goal_event = {};
    goal_event.etype = incoming.event_id;
    goal_event.payload_size = incoming.payload_size;
    goal_event.source_player_id = incoming.source_player_id;
    memcpy(goal_event.data, incoming.payload, incoming.payload_size);
  }
}
