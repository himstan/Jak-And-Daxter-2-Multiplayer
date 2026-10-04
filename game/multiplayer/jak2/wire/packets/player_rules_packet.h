#pragma once

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/quantization.h"

namespace multiplayer::jak2::wire {

struct PlayerRulesPacket : platform::wire::Packet<PlayerRulesPacket, PacketType::PLAYER_RULES> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "PLAYER_RULES",
      .direction = platform::MessageDirection::HOST_TO_CLIENT,
      .delivery = platform::Delivery::RELIABLE_ORDERED,
      .cadence = platform::CadenceMode::DIRTY,
      .maximum_payload_bytes = 4};

  uint16_t respawn_delay_seconds = core::kDefaultRespawnDelaySeconds;
  uint8_t player_collision = 0;
  uint8_t friendly_fire = 0;
};

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, PlayerRulesPacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  return platform::wire::serialize_u16(stream, packet.respawn_delay_seconds) &&
         platform::wire::serialize_u8(stream, packet.player_collision) &&
         platform::wire::serialize_u8(stream, packet.friendly_fire);
}

inline bool validate_packet(const PlayerRulesPacket& packet) {
  return packet.player_collision <= 1 && packet.friendly_fire <= 1;
}

inline PlayerRulesPacket to_packet(const core::PlayerRulesState& state) {
  PlayerRulesPacket packet;
  packet.respawn_delay_seconds = state.respawn_delay_seconds;
  packet.player_collision = state.player_collision;
  packet.friendly_fire = state.friendly_fire;
  return packet;
}

inline void from_packet(const PlayerRulesPacket& packet, core::PlayerRulesState& state) {
  state = {.respawn_delay_seconds = packet.respawn_delay_seconds,
           .player_collision = packet.player_collision != 0,
           .friendly_fire = packet.friendly_fire != 0};
}

inline void canonicalize_player_rules(core::PlayerRulesState&, const platform::MessageOrigin&) {}

}  // namespace multiplayer::jak2::wire
