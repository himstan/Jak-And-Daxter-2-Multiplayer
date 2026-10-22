#pragma once

#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/jak2/wire/player_data.h"

namespace multiplayer::jak2::wire {

struct PlayerDataPacket : platform::wire::Packet<PlayerDataPacket, PacketType::PLAYER_DATA> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "PLAYER_DATA",
      .direction = platform::MessageDirection::CLIENT_TO_HOST,
      .delivery = platform::Delivery::RELIABLE_ORDERED,
      .cadence = platform::CadenceMode::DIRTY,
      .maximum_payload_bytes = 29};

  uint32_t bootstrap_generation = 0;
  core::PlayerData data;
  bool operator==(const PlayerDataPacket& other) const {
    return bootstrap_generation == other.bootstrap_generation && data == other.data;
  }
};

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, PlayerDataPacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  return platform::wire::serialize_u32(stream, packet.bootstrap_generation) &&
         serialize_player_data(stream, packet.data);
}

inline bool validate_packet(const PlayerDataPacket& packet) {
  return packet.bootstrap_generation != 0 && core::valid_player_data(packet.data);
}

inline void from_packet(const PlayerDataPacket& packet, PlayerDataPacket& data) {
  data = packet;
}

}  // namespace multiplayer::jak2::wire
