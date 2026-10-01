#pragma once

#include "game/multiplayer/jak2/wire/packet_source.h"
#include "game/multiplayer/jak2/wire/packets/airlock_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/boss_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/enemy_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/game_event_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/pedestrian_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/player_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/player_vehicle_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/traffic_authority_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/turret_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/vehicle_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/world_state_packet.h"
#include "game/multiplayer/platform/wire/packet_codec.h"

namespace multiplayer::jak2::test {

template <typename Packet, typename Model, auto Canonicalize>
std::optional<Model> decode_body(std::span<const uint8_t> bytes,
                                 const platform::MessageOrigin& source) {
  const auto packet = platform::wire::decode_packet<Packet>(bytes);
  if (!packet || source.authenticated_player_id >= core::kMaxPlayers)
    return std::nullopt;
  Model model = {};
  wire::from_packet(*packet, model);
  Canonicalize(model, source);
  return model;
}

inline constexpr auto decode_player = decode_body<wire::PlayerStatePacket,
                                                  core::PlayerState,
                                                  wire::canonicalize_player<core::PlayerState>>;
inline constexpr auto decode_player_vehicle =
    decode_body<wire::PlayerVehicleStatePacket,
                core::PlayerVehicleState,
                wire::canonicalize_player<core::PlayerVehicleState>>;
inline constexpr auto decode_turret = decode_body<wire::TurretStatePacket,
                                                  core::TurretState,
                                                  wire::canonicalize_player<core::TurretState>>;
inline constexpr auto decode_events =
    decode_body<wire::GameEventBatchPacket, core::GameEventBatch, wire::canonicalize_events>;
inline constexpr auto decode_authority =
    decode_body<wire::TrafficAuthorityStatePacket,
                core::TrafficAuthority,
                wire::canonicalize_host_state<core::TrafficAuthority>>;
inline constexpr auto decode_enemies =
    decode_body<wire::EnemyStateBatchPacket,
                core::EnemySnapshot,
                wire::canonicalize_snapshot<core::EnemySnapshot>>;

}  // namespace multiplayer::jak2::test
