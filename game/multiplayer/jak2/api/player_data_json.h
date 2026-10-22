#pragma once

#include <optional>

#include "common/util/json_util.h"

#include "game/multiplayer/jak2/core/player_data.h"

namespace multiplayer::jak2 {

inline std::optional<core::PlayerData> read_player_data(const json& entry) {
  try {
    core::PlayerData data{
        .health = entry.at("health").get<float>(),
        .dark_eco = entry.value("dark_eco", 0.0f),
        .ammo = entry.value("ammo", std::array<float, 4>{}),
        .cosmetic_secrets = entry.value("cosmetic_secrets", 0u),
        .unlocked_cosmetic_secrets = entry.value("unlocked_cosmetic_secrets", 0u)};
    if (core::valid_player_data(data))
      return data;
  } catch (const std::exception&) {
  }
  return std::nullopt;
}

inline void merge_player_data(json& entry, const core::PlayerData& data) {
  entry["health"] = data.health;
  entry["dark_eco"] = data.dark_eco;
  entry["ammo"] = data.ammo;
  entry["cosmetic_secrets"] = data.cosmetic_secrets;
  entry["unlocked_cosmetic_secrets"] = data.unlocked_cosmetic_secrets;
}

}  // namespace multiplayer::jak2
