#include "game/multiplayer/jak2/api/player_data_store.h"

#include "common/log/log.h"

#include "game/multiplayer/jak2/api/player_data_json.h"

namespace multiplayer::jak2 {

PlayerDataStore::PlayerDataStore(fs::path directory) {
  if (directory.empty())
    return;
  path_ = directory / "player-data.json";
  try {
    if (!fs::exists(path_))
      return;
    const auto root = parse_commented_json(file_util::read_text_file(path_), path_.string());
    for (uint8_t slot = 0; slot < 4; ++slot) {
      const auto key = std::to_string(slot);
      if (!root.contains(key) || !root.at(key).is_object())
        continue;
      for (const auto& [id, entry] : root.at(key).items()) {
        const auto player_id = platform::PlayerId::parse(id);
        if (player_id && entry.is_object())
          saved_slots_[slot][player_id->to_string()] = entry;
      }
    }
  } catch (const std::exception& error) {
    lg::warn("[MP-Jak2] Could not load player data: {}", error.what());
  }
}

std::optional<core::PlayerData> PlayerDataStore::get(const platform::PlayerId& player_id) const {
  const auto player = players_.find(player_id.to_string());
  return player == players_.end() ? std::nullopt : read_player_data(player->second);
}

bool PlayerDataStore::stage_save(uint8_t save_slot) {
  pending_save_.reset();
  if (save_slot >= 4)
    return false;
  pending_save_ = {players_, save_slot};
  return true;
}

bool PlayerDataStore::commit_save(uint8_t save_slot) {
  if (!pending_save_ || pending_save_->save_slot != save_slot)
    return false;
  auto saved = saved_slots_;
  saved[save_slot] = pending_save_->players;
  try {
    if (!path_.empty()) {
      json root = json::object();
      for (const auto& [slot, players] : saved)
        root[std::to_string(slot)] = players;
      file_util::write_text_file_atomic(path_, root.dump(2) + "\n");
    }
  } catch (const std::exception& error) {
    lg::warn("[MP-Jak2] Could not save player data: {}", error.what());
    return false;
  }
  saved_slots_ = std::move(saved);
  pending_save_.reset();
  return true;
}

void PlayerDataStore::load(uint8_t save_slot) {
  const auto slot = saved_slots_.find(save_slot);
  players_ = slot == saved_slots_.end() ? Players{} : slot->second;
  pending_save_.reset();
}

void PlayerDataStore::update(const platform::PlayerId& player_id,
                             const core::PlayerData& player_data) {
  if (player_id.valid() && core::valid_player_data(player_data))
    merge_player_data(players_[player_id.to_string()], player_data);
}

}  // namespace multiplayer::jak2
