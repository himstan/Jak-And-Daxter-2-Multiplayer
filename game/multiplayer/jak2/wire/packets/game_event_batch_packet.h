#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/platform/wire/packet_codec.h"

namespace multiplayer::jak2::wire {

inline constexpr size_t kGameEventBatchPacketPrefixWireSize = 1;
inline constexpr size_t kGameEventRecordPrefixWireSize = 2;

struct GameEventRecord {
  uint8_t event_id = 0;
  uint8_t payload_size = 0;
  std::array<uint8_t, 64> payload = {};
};

struct GameEventBatchPacket
    : platform::wire::Packet<GameEventBatchPacket, PacketType::GAME_EVENT_BATCH> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "GAME_EVENT_BATCH",
      .delivery = platform::Delivery::RELIABLE_ORDERED,
      .maximum_payload_bytes = 8192};

  std::vector<GameEventRecord> events;
};

bool valid_event(const GameEventRecord& event);

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, GameEventBatchPacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  } else {
    if (packet.events.empty() || packet.events.size() > UINT8_MAX) {
      return false;
    }
    for (auto& event : packet.events) {
      if (!valid_event(event)) {
        return false;
      }
    }
  }

  uint8_t count = static_cast<uint8_t>(packet.events.size());
  if (!platform::wire::serialize_u8(stream, count)) {
    return false;
  }
  if constexpr (Stream::IsReading) {
    if (count == 0) {
      return false;
    }
    packet.events.reserve(count);
    for (uint8_t index = 0; index < count; ++index) {
      GameEventRecord event;
      if (!platform::wire::serialize_u8(stream, event.event_id) ||
          !platform::wire::serialize_u8(stream, event.payload_size)) {
        return false;
      }
      if (!valid_event(event) || !platform::wire::serialization::serialize_raw_bytes(
                                     stream, event.payload.data(), event.payload_size)) {
        return false;
      }
      packet.events.push_back(event);
    }
  } else {
    for (auto& event : packet.events) {
      if (!platform::wire::serialize_u8(stream, event.event_id) ||
          !platform::wire::serialize_u8(stream, event.payload_size) ||
          !platform::wire::serialization::serialize_raw_bytes(stream, event.payload.data(),
                                                              event.payload_size)) {
        return false;
      }
    }
  }
  return platform::wire::serialize_byte_align(stream);
}

bool validate_packet(const GameEventBatchPacket& packet);

GameEventBatchPacket to_packet(const core::GameEventBatch& batch);
void from_packet(const GameEventBatchPacket& packet, core::GameEventBatch& batch);

}  // namespace multiplayer::jak2::wire
