#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace multiplayer::jak2::core {

inline constexpr uint32_t kPlayerCosmeticSecretsMask = 0x00030003u;

struct PlayerData {
  float health = 0.0f;
  float dark_eco = 0.0f;
  std::array<float, 4> ammo = {};
  uint32_t cosmetic_secrets = 0;
  uint32_t unlocked_cosmetic_secrets = 0;

  bool operator==(const PlayerData&) const = default;
};

inline bool valid_player_data(const PlayerData& player_data) {
  const auto valid = [](float value) { return std::isfinite(value) && value >= 0.0f; };
  return ((player_data.cosmetic_secrets | player_data.unlocked_cosmetic_secrets) &
          ~kPlayerCosmeticSecretsMask) == 0 &&
         valid(player_data.health) && valid(player_data.dark_eco) &&
         std::ranges::all_of(player_data.ammo, valid);
}

}  // namespace multiplayer::jak2::core
