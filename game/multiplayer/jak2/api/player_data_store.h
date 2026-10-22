#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

#include "game/multiplayer/jak2/core/player_data.h"
#include "game/multiplayer/platform/core/player_id.h"

namespace multiplayer::jak2 {

enum class PlayerDataSaveAction : uint8_t { CAPTURE, COMMIT, LOAD };

class PlayerDataStore {
 public:
  explicit PlayerDataStore(fs::path directory = {});
  bool stage_save(uint8_t save_slot);
  bool commit_save(uint8_t save_slot);
  void load(uint8_t save_slot);
  std::optional<core::PlayerData> get(const platform::PlayerId& player_id) const;
  void update(const platform::PlayerId& player_id, const core::PlayerData& player_data);

 private:
  using Players = std::map<std::string, json>;
  using Slots = std::map<uint8_t, Players>;
  struct PendingSave {
    Players players;
    uint8_t save_slot;
  };
  fs::path path_;
  Players players_;
  Slots saved_slots_;
  std::optional<PendingSave> pending_save_;
};

}  // namespace multiplayer::jak2
