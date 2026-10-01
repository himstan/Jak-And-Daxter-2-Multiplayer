#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>

#include "game/multiplayer/jak2/application/replication_mailbox.h"
#include "game/multiplayer/jak2/core/replication_state.h"
#include "game/multiplayer/platform/replication/snapshot_timeline.h"

namespace multiplayer::jak2::application {

inline constexpr float kGoalUnitsPerMeter = 4096.0f;

class PresentationRuntime {
 public:
  void reset_player(core::PlayerId player_id);
  void prepare(RemoteReplicationFrame& frame, const core::ReplicationState& state, uint64_t now_ms);
  void reset();

 private:
  struct TimelineState {
    core::Sequence sequence = 0;
    uint32_t generation = 0;
    uint32_t identity = 0;
    uint32_t context = 0;
    uint32_t level_id = 0;
    core::PlayerId source = core::kInvalidPlayerId;
    platform::replication::SnapshotTimeline timeline;
  };

  PresentationTarget prepare_player(core::PlayerId player_id,
                                    const core::PlayerState& player,
                                    uint64_t now_ms);
  PresentationTarget prepare_player_vehicle(core::PlayerId player_id,
                                            const core::PlayerVehicleState& state,
                                            const core::PlayerState& player,
                                            uint64_t now_ms);
  void prepare_enemies(RemoteReplicationFrame& frame, uint64_t now_ms);
  void prepare_traffic(RemoteReplicationFrame& frame, uint64_t now_ms);
  void prepare_bosses(RemoteReplicationFrame& frame, uint64_t now_ms);
  void update_packet_generations(const core::ReplicationState& state);

  std::array<TimelineState, core::kMaxPlayers> player_histories_ = {};
  std::array<TimelineState, core::kMaxPlayers> player_vehicle_histories_ = {};
  std::unordered_map<core::EntityId, TimelineState> enemy_histories_;
  std::unordered_map<core::EntityId, TimelineState> pedestrian_histories_;
  std::unordered_map<core::EntityId, TimelineState> ambient_vehicle_histories_;
  std::array<TimelineState, 2> boss_histories_ = {};
  uint32_t enemy_generation_ = 0;
  uint32_t traffic_generation_ = 0;
  std::array<core::Sequence, core::kMaxPlayers> enemy_source_sequences_ = {};
  core::PlayerId traffic_signature_source_ = core::kInvalidPlayerId;
  core::Sequence pedestrian_signature_sequence_ = 0;
  core::Sequence vehicle_signature_sequence_ = 0;
};

}  // namespace multiplayer::jak2::application
