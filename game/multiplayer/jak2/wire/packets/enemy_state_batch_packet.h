#pragma once

#include <array>
#include <vector>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/jak2/wire/multiplayer_protocol.h"
#include "game/multiplayer/jak2/wire/wire_policy.h"
#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/quantization.h"

namespace multiplayer::jak2::wire {

struct EnemyStateRecord {
  uint32_t actor_id = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {};
  int32_t hit_points = 0;
  uint32_t state_id = 0;
  uint8_t focus_player_id = kWireInvalidPlayerId;
  uint8_t flags = 0;
};

struct EnemyStateBatchPacket
    : platform::wire::Packet<EnemyStateBatchPacket, PacketType::ENEMY_STATE_BATCH> {
  static constexpr platform::GameMessagePolicy kPolicy = {
      .id = static_cast<uint8_t>(kPacketType),
      .name = "ENEMY_STATE_BATCH",
      .priority = platform::MessagePriority::BULK,
      .cadence = platform::CadenceMode::PERIODIC,
      .interval_ms = 50,
      .maximum_payload_bytes = 2965};

  uint32_t sample_time_ms = 0;
  std::vector<EnemyStateRecord> enemies;
};

template <typename Stream, typename RecordT>
bool serialize_enemy_state_record(Stream& stream, RecordT&& enemy) {
  if constexpr (Stream::IsReading) {
    enemy = {};
  }
  if (!platform::wire::serialize_u32(stream, enemy.actor_id) ||
      !platform::wire::serialize_position_array(stream, enemy.position) ||
      !platform::wire::serialize_quaternion_array(stream, enemy.quaternion) ||
      !platform::wire::serialization::serialize_i32(stream, enemy.hit_points) ||
      !platform::wire::serialize_uint_bits(stream, enemy.state_id, 7) ||
      !platform::wire::serialize_uint_bits(stream, enemy.focus_player_id, 4) ||
      !platform::wire::serialize_uint_bits(stream, enemy.flags, 2)) {
    return false;
  }
  return valid_wire_player_reference(enemy.focus_player_id);
}

template <typename Stream, typename PacketT>
  requires(std::is_same_v<std::remove_cvref_t<PacketT>, EnemyStateBatchPacket>)
bool serialize_fields(Stream& stream, PacketT&& packet) {
  if constexpr (Stream::IsReading) {
    packet = {};
  } else if (packet.enemies.size() > core::kMaxEnemies) {
    return false;
  }

  if (!platform::wire::serialize_u32(stream, packet.sample_time_ms)) {
    return false;
  }
  uint8_t count = static_cast<uint8_t>(packet.enemies.size());
  if (!platform::wire::serialize_u8(stream, count)) {
    return false;
  }
  if constexpr (Stream::IsReading) {
    if (count > core::kMaxEnemies) {
      return false;
    }
    packet.enemies.reserve(count);
    for (uint8_t index = 0; index < count; ++index) {
      EnemyStateRecord enemy;
      if (!serialize_enemy_state_record(stream, enemy)) {
        return false;
      }
      packet.enemies.push_back(enemy);
    }
  } else {
    for (auto& enemy : packet.enemies) {
      if (!serialize_enemy_state_record(stream, enemy)) {
        return false;
      }
    }
  }
  return platform::wire::serialize_byte_align(stream);
}

bool validate_packet(const EnemyStateBatchPacket& packet);

EnemyStateBatchPacket to_packet(const core::EnemySnapshot& snapshot);
void from_packet(const EnemyStateBatchPacket& packet, core::EnemySnapshot& snapshot);

}  // namespace multiplayer::jak2::wire
