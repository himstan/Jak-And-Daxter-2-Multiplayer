#pragma once

#include <array>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/jak2/wire/wire_policy.h"
#include "game/multiplayer/platform/wire/packet_codec.h"

namespace multiplayer::jak2::wire {

inline constexpr size_t kTrafficAuthorityStatePacketWireSize = 8;

struct TrafficAuthorityStatePacket
    : platform::wire::Packet<TrafficAuthorityStatePacket, PacketType::TRAFFIC_AUTHORITY_STATE> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "TRAFFIC_AUTHORITY_STATE",
      .direction = platform::MessageDirection::HOST_TO_CLIENT,
      .delivery = platform::Delivery::RELIABLE_ORDERED,
      .cadence = platform::CadenceMode::DIRTY,
      .maximum_payload_bytes = 8};

  uint32_t revision = 0;
  std::array<uint8_t, core::kMaxPlayers> assignments = {};
};

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, TrafficAuthorityStatePacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  }
  if (!platform::wire::serialize_u32(stream, packet.revision)) {
    return false;
  }
  for (auto& assignment : packet.assignments) {
    if (!platform::wire::serialize_uint_bits(stream, assignment, 4) ||
        !valid_wire_player_reference(assignment)) {
      return false;
    }
  }
  return platform::wire::serialize_byte_align(stream);
}

bool validate_packet(const TrafficAuthorityStatePacket& packet);

TrafficAuthorityStatePacket to_packet(const core::TrafficAuthority& authority);
void from_packet(const TrafficAuthorityStatePacket& packet, core::TrafficAuthority& authority);

}  // namespace multiplayer::jak2::wire
