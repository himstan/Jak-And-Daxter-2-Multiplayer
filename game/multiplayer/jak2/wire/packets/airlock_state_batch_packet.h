#pragma once

#include <vector>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/platform/wire/packet_codec.h"

namespace multiplayer::jak2::wire {

inline constexpr size_t kAirlockStateBatchPacketPrefixWireSize = 1;
inline constexpr size_t kAirlockStateRecordWireBits = 82;
inline constexpr size_t kAirlockStateBatchPacketMaxWireSize = 42;

struct AirlockStateRecord {
  uint32_t airlock_aid = 0;
  uint32_t state_id = 0;
  uint32_t level_id = 0;
  uint32_t sequence = 0;
};

struct AirlockStateBatchPacket
    : platform::wire::Packet<AirlockStateBatchPacket, PacketType::AIRLOCK_STATE_BATCH> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "AIRLOCK_STATE_BATCH",
      .cadence = platform::CadenceMode::PERIODIC,
      .interval_ms = 100,
      .maximum_payload_bytes = 42};

  std::vector<AirlockStateRecord> states;
};

template <typename Stream, typename RecordT>
bool serialize_airlock_state_record(Stream& stream, RecordT&& state) {
  if constexpr (Stream::IsReading) {
    state = {};
  }
  if (!platform::wire::serialize_u32(stream, state.airlock_aid) ||
      !platform::wire::serialize_uint_bits(stream, state.state_id, 2) ||
      !platform::wire::serialize_uint_bits(stream, state.level_id, 16) ||
      !platform::wire::serialize_u32(stream, state.sequence)) {
    return false;
  }
  return state.airlock_aid != 0 && state.state_id <= 3u && state.level_id <= 0xffffu;
}

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, AirlockStateBatchPacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  } else {
    if (packet.states.size() > core::kMaxAirlockStatesPerSnapshot) {
      return false;
    }
    for (const auto& state : packet.states) {
      if (state.airlock_aid == 0 || state.state_id > 3u || state.level_id > 0xffffu) {
        return false;
      }
    }
  }

  uint8_t count = static_cast<uint8_t>(packet.states.size());
  if (!platform::wire::serialize_uint_bits(stream, count, 3)) {
    return false;
  }
  if constexpr (Stream::IsReading) {
    if (count > core::kMaxAirlockStatesPerSnapshot) {
      return false;
    }
    packet.states.reserve(count);
    for (uint8_t index = 0; index < count; ++index) {
      AirlockStateRecord state;
      if (!serialize_airlock_state_record(stream, state)) {
        return false;
      }
      packet.states.push_back(state);
    }
  } else {
    for (auto& state : packet.states) {
      if (!serialize_airlock_state_record(stream, state)) {
        return false;
      }
    }
  }
  return platform::wire::serialize_byte_align(stream);
}

bool validate_packet(const AirlockStateBatchPacket& packet);

AirlockStateBatchPacket to_packet(const core::AirlockSnapshot& snapshot);
void from_packet(const AirlockStateBatchPacket& packet, core::AirlockSnapshot& snapshot);

}  // namespace multiplayer::jak2::wire
