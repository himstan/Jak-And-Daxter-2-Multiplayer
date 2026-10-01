#pragma once

#include "game/multiplayer/jak2/core/multiplayer_types.h"

namespace multiplayer::jak2::wire {

template <typename Model>
void canonicalize_player(Model& model, const platform::MessageOrigin& source) {
  model.player_id = source.authenticated_player_id;
}

template <typename Model>
void canonicalize_snapshot(Model& model, const platform::MessageOrigin& source) {
  model.source_player_id = source.authenticated_player_id;
}

inline void canonicalize_events(core::GameEventBatch& batch,
                                const platform::MessageOrigin& source) {
  for (auto& event : batch.events)
    event.source_player_id = source.authenticated_player_id;
}

template <typename Model>
void canonicalize_host_state(Model&, const platform::MessageOrigin&) {}

}  // namespace multiplayer::jak2::wire
