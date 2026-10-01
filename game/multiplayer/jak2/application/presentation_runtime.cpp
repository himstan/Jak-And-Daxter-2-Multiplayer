#include "game/multiplayer/jak2/application/presentation_runtime.h"

#include <cmath>
#include <unordered_set>

#include "game/multiplayer/jak2/core/validation.h"

namespace multiplayer::jak2::application {
namespace {

struct TimingProfile {
  uint32_t nominal_interval_ms;
  uint32_t minimum_delay_ms;
  uint32_t maximum_delay_ms;
  uint32_t maximum_extrapolation_ms;
  float output_smoothing_rate = 0.0f;
};

constexpr TimingProfile kCriticalProfile = {.nominal_interval_ms = 33,
                                            .minimum_delay_ms = 33,
                                            .maximum_delay_ms = 150,
                                            .maximum_extrapolation_ms = 250};
constexpr TimingProfile kEnemyProfile = {.nominal_interval_ms = 50,
                                         .minimum_delay_ms = 66,
                                         .maximum_delay_ms = 400,
                                         .maximum_extrapolation_ms = 400};
constexpr TimingProfile kTrafficProfile = {.nominal_interval_ms = 66,
                                           .minimum_delay_ms = 66,
                                           .maximum_delay_ms = 400,
                                           .maximum_extrapolation_ms = 400};
constexpr TimingProfile kBossProfile = {
    .nominal_interval_ms = 33,
    .minimum_delay_ms = 50,
    .maximum_delay_ms = 200,
    .maximum_extrapolation_ms = 250,
    .output_smoothing_rate = 12.0f,
};

platform::replication::PresentedTransform present_timeline(
    platform::replication::SnapshotTimeline& timeline,
    const uint64_t now_ms,
    const TimingProfile& profile) {
  return timeline.present(now_ms, profile.nominal_interval_ms, profile.minimum_delay_ms,
                          profile.maximum_delay_ms, profile.maximum_extrapolation_ms,
                          profile.output_smoothing_rate);
}

PresentationTarget make_target(const uint32_t entity_id,
                               const platform::replication::PresentedTransform& presented,
                               const uint32_t generation) {
  return {.entity_id = entity_id,
          .position = presented.position,
          .quaternion = presented.quaternion,
          .velocity = presented.velocity,
          .generation = generation,
          .valid = presented.valid};
}

}  // namespace

void PresentationRuntime::reset() {
  player_histories_ = {};
  player_vehicle_histories_ = {};
  enemy_histories_.clear();
  pedestrian_histories_.clear();
  ambient_vehicle_histories_.clear();
  boss_histories_ = {};
  enemy_generation_ = 0;
  traffic_generation_ = 0;
  enemy_source_sequences_ = {};
  traffic_signature_source_ = core::kInvalidPlayerId;
  pedestrian_signature_sequence_ = 0;
  vehicle_signature_sequence_ = 0;
}

void PresentationRuntime::reset_player(const core::PlayerId player_id) {
  if (player_id >= core::kMaxPlayers)
    return;
  player_histories_[player_id] = {};
  player_vehicle_histories_[player_id] = {};
}

PresentationTarget PresentationRuntime::prepare_player(const core::PlayerId player_id,
                                                       const core::PlayerState& player,
                                                       const uint64_t now_ms) {
  auto& history = player_histories_[player_id];
  if (!player.state_ready || player.last_sequence == 0) {
    history = {};
    return {};
  }
  const uint32_t primary_level = core::primary_level_id(player.levels);
  const bool reset = history.sequence != 0 && history.level_id != primary_level;
  if (reset)
    history.timeline.reset();
  if (reset || history.sequence != player.last_sequence) {
    constexpr float kRotToRad = 3.14159265358979323846f / 32768.0f;
    const float half_angle = player.angle * kRotToRad * 0.5f;
    history.timeline.push({.sample_time_ms = player.sample_time_ms,
                           .position = player.position,
                           .quaternion = {0.0f, std::sin(half_angle), 0.0f, std::cos(half_angle)},
                           .velocity = player.velocity,
                           .velocity_valid = true},
                          player.received_time_ms, 20.0f * kGoalUnitsPerMeter);
    history.sequence = player.last_sequence;
    history.level_id = primary_level;
    ++history.generation;
  }
  return make_target(player_id, present_timeline(history.timeline, now_ms, kCriticalProfile),
                     history.generation);
}

PresentationTarget PresentationRuntime::prepare_player_vehicle(
    const core::PlayerId player_id,
    const core::PlayerVehicleState& state,
    const core::PlayerState& player,
    const uint64_t now_ms) {
  const auto& vehicle = state.vehicle;
  auto& history = player_vehicle_histories_[player_id];
  if (state.player_id != player_id || vehicle.net_id == 0 || vehicle.last_sequence == 0) {
    history = {};
    return {};
  }
  const uint32_t primary_level = core::primary_level_id(player.levels);
  const bool reset = (history.identity != 0 && history.identity != vehicle.net_id) ||
                     (history.level_id != 0 && history.level_id != primary_level);
  if (reset)
    history.timeline.reset();
  if (reset || history.sequence != vehicle.last_sequence) {
    history.timeline.push({.sample_time_ms = vehicle.sample_time_ms,
                           .position = vehicle.position,
                           .quaternion = vehicle.quaternion,
                           .velocity = vehicle.linear_velocity,
                           .velocity_valid = true},
                          vehicle.received_time_ms, 15.0f * kGoalUnitsPerMeter);
    history.sequence = vehicle.last_sequence;
    history.identity = vehicle.net_id;
    history.level_id = primary_level;
    ++history.generation;
  }
  return make_target(vehicle.net_id, present_timeline(history.timeline, now_ms, kCriticalProfile),
                     history.generation);
}

void PresentationRuntime::prepare_enemies(RemoteReplicationFrame& frame, const uint64_t now_ms) {
  std::unordered_set<core::EntityId> present;
  frame.enemy_targets.clear();
  frame.enemy_targets.reserve(frame.enemies.enemies.size());
  for (const auto& enemy : frame.enemies.enemies) {
    present.insert(enemy.actor_id);
    auto& history = enemy_histories_[enemy.actor_id];
    const uint32_t level_id =
        core::valid_index(enemy.owner_player_id, core::kMaxPlayers)
            ? core::primary_level_id(frame.players[enemy.owner_player_id].levels)
            : 0;
    const bool reset =
        (history.source != core::kInvalidPlayerId && history.source != enemy.owner_player_id) ||
        (history.level_id != 0 && history.level_id != level_id);
    if (reset)
      history.timeline.reset();
    if (reset || history.sequence != enemy.last_sequence) {
      history.timeline.push({.sample_time_ms = enemy.sample_time_ms,
                             .position = enemy.position,
                             .quaternion = enemy.quaternion},
                            enemy.received_time_ms, 5.0f * kGoalUnitsPerMeter);
      history.sequence = enemy.last_sequence;
      history.level_id = level_id;
      history.source = enemy.owner_player_id;
      ++history.generation;
    }
    frame.enemy_targets.push_back(
        make_target(enemy.actor_id, present_timeline(history.timeline, now_ms, kEnemyProfile),
                    history.generation));
  }
  std::erase_if(enemy_histories_, [&](const auto& item) { return !present.contains(item.first); });
}

void PresentationRuntime::prepare_traffic(RemoteReplicationFrame& frame, const uint64_t now_ms) {
  const auto prepare = [&](const auto& entities, auto& histories, auto& targets,
                           const bool velocity_valid, const float vertical_extrapolation_scale) {
    std::unordered_set<core::EntityId> present;
    targets.clear();
    targets.reserve(entities.size());
    for (const auto& entity : entities) {
      present.insert(entity.net_id);
      auto& history = histories[entity.net_id];
      const bool reset =
          (history.context != 0 && history.context != frame.selected_traffic.authority_revision) ||
          (history.level_id != 0 && history.level_id != frame.selected_traffic.level_id) ||
          (history.source != core::kInvalidPlayerId &&
           history.source != frame.selected_traffic.source_player_id);
      if (reset)
        history.timeline.reset();
      if (reset || history.sequence != entity.last_sequence) {
        std::array<float, 3> velocity = {};
        if constexpr (requires { entity.linear_velocity; })
          velocity = entity.linear_velocity;
        history.timeline.push({.sample_time_ms = entity.sample_time_ms,
                               .position = entity.position,
                               .quaternion = entity.quaternion,
                               .velocity = velocity,
                               .velocity_valid = velocity_valid},
                              entity.received_time_ms, 15.0f * kGoalUnitsPerMeter);
        history.sequence = entity.last_sequence;
        history.context = frame.selected_traffic.authority_revision;
        history.level_id = frame.selected_traffic.level_id;
        history.source = frame.selected_traffic.source_player_id;
        ++history.generation;
      }
      targets.push_back(make_target(entity.net_id,
                                    present_timeline(history.timeline, now_ms, kTrafficProfile),
                                    history.generation));
    }
    std::erase_if(histories, [&](const auto& item) { return !present.contains(item.first); });
  };
  prepare(frame.selected_traffic.pedestrians, pedestrian_histories_, frame.pedestrian_targets,
          false, 1.0f);

  prepare(frame.selected_traffic.vehicles, ambient_vehicle_histories_,
          frame.ambient_vehicle_targets, true, 1.0f);
}

void PresentationRuntime::prepare_bosses(RemoteReplicationFrame& frame, const uint64_t now_ms) {
  for (size_t index = 0; index < frame.bosses.size(); ++index) {
    const auto& boss = frame.bosses[index];
    auto& history = boss_histories_[index];
    if (boss.active == 0 || boss.sequence == 0) {
      history = {};
      frame.boss_targets[index] = {};
      continue;
    }
    const uint32_t context = boss.active ^ (static_cast<uint32_t>(boss.stage) * 16777619u);
    const bool reset = history.sequence != 0 && history.context != context;
    if (reset)
      history.timeline.reset();
    if (reset || history.sequence != boss.sequence) {
      const bool squid = boss.kind == core::BossState::Kind::PALACE_SQUID;
      history.timeline.push({.sample_time_ms = boss.sample_time_ms,
                             .position = squid ? boss.root_position : boss.position,
                             .quaternion = squid ? boss.root_quaternion : boss.quaternion},
                            boss.received_time_ms, 20.0f * kGoalUnitsPerMeter);
      history.sequence = boss.sequence;
      history.context = context;
      ++history.generation;
    }
    frame.boss_targets[index] =
        make_target(static_cast<uint32_t>(index + 1),
                    present_timeline(history.timeline, now_ms, kBossProfile), history.generation);
  }
}

void PresentationRuntime::update_packet_generations(const core::ReplicationState& state) {
  bool enemy_changed = false;
  for (core::PlayerId source = 0; source < core::kMaxPlayers; ++source) {
    if (const auto sequence = state.entities().enemy_snapshot(source).sequence;
        sequence != enemy_source_sequences_[source]) {
      enemy_source_sequences_[source] = sequence;
      enemy_changed = true;
    }
  }
  if (enemy_changed)
    ++enemy_generation_;

  const auto source = state.traffic().selected_authority();
  const auto pedestrian_sequence = core::valid_index(source, core::kMaxPlayers)
                                       ? state.traffic().pedestrian_snapshot(source).sequence
                                       : 0;
  const auto vehicle_sequence = core::valid_index(source, core::kMaxPlayers)
                                    ? state.traffic().vehicle_snapshot(source).sequence
                                    : 0;
  if (source != traffic_signature_source_ ||
      pedestrian_sequence != pedestrian_signature_sequence_ ||
      vehicle_sequence != vehicle_signature_sequence_) {
    traffic_signature_source_ = source;
    pedestrian_signature_sequence_ = pedestrian_sequence;
    vehicle_signature_sequence_ = vehicle_sequence;
    ++traffic_generation_;
  }
}

void PresentationRuntime::prepare(RemoteReplicationFrame& frame,
                                  const core::ReplicationState& state,
                                  const uint64_t now_ms) {
  update_packet_generations(state);
  for (core::PlayerId player_id = 0; player_id < core::kMaxPlayers; ++player_id) {
    frame.player_targets[player_id] = prepare_player(player_id, frame.players[player_id], now_ms);
    frame.player_vehicle_targets[player_id] = prepare_player_vehicle(
        player_id, frame.player_vehicles[player_id], frame.players[player_id], now_ms);
  }
  prepare_enemies(frame, now_ms);
  prepare_traffic(frame, now_ms);
  prepare_bosses(frame, now_ms);
  frame.enemy_generation = enemy_generation_;
  frame.traffic_generation = traffic_generation_;
}

}  // namespace multiplayer::jak2::application
