#pragma once

#include <optional>

#include "game/multiplayer/jak2/core/player_data.h"
#include "game/multiplayer/platform/wire/packet_codec.h"

namespace multiplayer::jak2::wire {

template <typename Stream>
bool serialize_cosmetic_secrets(Stream& stream, uint32_t& secrets) {
  uint8_t bits = (secrets & 3u) | ((secrets >> 14) & 12u);
  if (!platform::wire::serialize_uint_bits(stream, bits, 4))
    return false;
  if constexpr (Stream::IsReading)
    secrets = (bits & 3u) | ((bits & 12u) << 14);
  return true;
}

template <typename Stream, typename PlayerData>
bool serialize_player_data(Stream& stream, PlayerData& player_data) {
  if (!platform::wire::serialization::serialize_f32(stream, player_data.health) ||
      !platform::wire::serialization::serialize_f32(stream, player_data.dark_eco))
    return false;
  for (auto& ammo : player_data.ammo) {
    if (!platform::wire::serialization::serialize_f32(stream, ammo))
      return false;
  }
  return serialize_cosmetic_secrets(stream, player_data.cosmetic_secrets) &&
         serialize_cosmetic_secrets(stream, player_data.unlocked_cosmetic_secrets);
}

template <typename Stream, typename Optional>
bool serialize_optional_player_data(Stream& stream, Optional& data) {
  uint8_t present = data.has_value();
  if (!platform::wire::serialize_u8(stream, present) || present > 1)
    return false;
  if constexpr (Stream::IsReading) {
    if (present)
      data.emplace();
    else
      data.reset();
  }
  return !present || serialize_player_data(stream, *data);
}

}  // namespace multiplayer::jak2::wire
