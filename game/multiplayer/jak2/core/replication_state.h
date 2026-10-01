#pragma once

#include <array>
#include <deque>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/platform/session/game_adapter.h"

namespace multiplayer::jak2::core {

using ApplyContext = platform::PacketContext;

class ParticipantReplicationState {
 public:
  void reset();
  void expire(uint64_t now_ms);
  bool apply(const PlayerState& state, const ApplyContext& context);
  bool apply(const PlayerVehicleState& state, const ApplyContext& context);
  bool apply(const TurretState& state, const ApplyContext& context);
  bool update_identity(const PlayerIdentity& identity);
  void depart(PlayerId player_id);

  const auto& identities() const { return identities_; }
  const auto& players() const { return players_; }
  const auto& player_vehicles() const { return player_vehicles_; }
  const auto& turrets() const { return turrets_; }
  const auto& turret_sequences() const { return turret_sequences_; }

 private:
  std::array<PlayerIdentity, kMaxPlayers> identities_ = {};
  std::array<PlayerState, kMaxPlayers> players_ = {};
  std::array<PlayerVehicleState, kMaxPlayers> player_vehicles_ = {};
  std::array<Sequence, kMaxPlayers> player_vehicle_sequences_ = {};
  std::array<TurretState, kMaxPlayers> turrets_ = {};
  std::array<Sequence, kMaxPlayers> turret_sequences_ = {};
};

class TrafficReplicationState {
 public:
  void reset();
  void expire(uint64_t now_ms);
  bool apply(const TrafficAuthority& authority, const ApplyContext& context);
  bool apply(const TrafficSnapshot& snapshot, const ApplyContext& context);
  bool select_authority(PlayerId source_player_id);
  void clear();
  void clear_source(PlayerId source_player_id);
  void clear_source(PlayerId source_player_id, TrafficSnapshot::Kind kind);

  const TrafficAuthority& authority() const { return authority_; }
  PlayerId selected_authority() const { return selected_authority_; }
  const TrafficSnapshot& aggregate() const { return aggregate_; }
  const TrafficSnapshot& selected_snapshot() const { return selected_; }
  const TrafficSnapshot& pedestrian_snapshot(PlayerId source_player_id) const {
    return pedestrian_snapshots_[source_player_id];
  }
  const TrafficSnapshot& vehicle_snapshot(PlayerId source_player_id) const {
    return vehicle_snapshots_[source_player_id];
  }
  PlayerId vehicle_source(EntityId net_id) const;

 private:
  void prepare_level(PlayerId source_player_id, uint32_t level_id);
  void rebuild_aggregate();
  void rebuild_selected();

  TrafficAuthority authority_ = {};
  PlayerId selected_authority_ = kInvalidPlayerId;
  TrafficSnapshot aggregate_ = {};
  TrafficSnapshot selected_ = {};
  std::array<Sequence, kMaxPlayers> pedestrian_sequences_ = {};
  std::array<Sequence, kMaxPlayers> vehicle_sequences_ = {};
  std::array<TrafficSnapshot, kMaxPlayers> pedestrian_snapshots_ = {};
  std::array<TrafficSnapshot, kMaxPlayers> vehicle_snapshots_ = {};
};

class WorldReplicationState {
 public:
  void reset();
  bool apply(const WorldState& state, const ApplyContext& context);
  bool apply_bootstrap(const BootstrapState& state, Sequence sequence);

  const WorldState& world() const { return world_; }
  const BootstrapState& bootstrap() const { return bootstrap_; }

 private:
  WorldState world_ = {};
  BootstrapState bootstrap_ = {};
};

class EntityReplicationState {
 public:
  void reset();
  void expire(uint64_t now_ms);
  bool apply(const EnemySnapshot& snapshot, const ApplyContext& context);
  bool apply(const BossState& state, const ApplyContext& context);
  bool apply(const AirlockSnapshot& snapshot, const ApplyContext& context);
  void depart(PlayerId player_id);
  void clear_enemy_source(PlayerId player_id);

  const EnemySnapshot& enemies() const { return enemies_; }
  const EnemySnapshot& enemy_snapshot(PlayerId source_player_id) const {
    return enemy_snapshots_[source_player_id];
  }
  const BossState& boss(BossState::Kind kind) const { return bosses_[static_cast<size_t>(kind)]; }
  const std::array<AirlockSnapshot, kMaxPlayers>& airlocks() const { return airlock_snapshots_; }

 private:
  void rebuild_enemies();

  EnemySnapshot enemies_ = {};
  std::array<EnemySnapshot, kMaxPlayers> enemy_snapshots_ = {};
  std::array<BossState, 2> bosses_ = {};
  std::array<AirlockSnapshot, kMaxPlayers> airlock_snapshots_ = {};
  std::array<Sequence, kMaxPlayers> airlock_sequences_ = {};
};

class EventReplicationState {
 public:
  void reset();
  bool apply(const GameEventBatch& batch, const ApplyContext& context);
  void depart(PlayerId player_id);
  std::vector<GameEvent> take(size_t maximum);
  const std::deque<GameEvent>& events() const { return events_; }

 private:
  std::array<Sequence, kMaxPlayers> sequences_ = {};
  std::deque<GameEvent> events_;
};

class ReplicationState {
 public:
  void reset();
  void expire(uint64_t now_ms);
  bool apply_bootstrap(const BootstrapState& state, Sequence sequence);
  bool update_participant_identity(const PlayerIdentity& identity);
  bool depart_participant(PlayerId player_id);

  ParticipantReplicationState& participants() { return participants_; }
  const ParticipantReplicationState& participants() const { return participants_; }
  TrafficReplicationState& traffic() { return traffic_; }
  const TrafficReplicationState& traffic() const { return traffic_; }
  WorldReplicationState& world() { return world_; }
  const WorldReplicationState& world() const { return world_; }
  EntityReplicationState& entities() { return entities_; }
  const EntityReplicationState& entities() const { return entities_; }
  EventReplicationState& events() { return events_; }
  const EventReplicationState& events() const { return events_; }

 private:
  ParticipantReplicationState participants_;
  TrafficReplicationState traffic_;
  WorldReplicationState world_;
  EntityReplicationState entities_;
  EventReplicationState events_;
};

}  // namespace multiplayer::jak2::core
