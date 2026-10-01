#include <algorithm>
#include <limits>

#include "game/multiplayer/jak2/application/jak2_adapter.h"
#include "game/multiplayer/jak2/core/validation.h"
#include "game/multiplayer/jak2/wire/packet_source.h"
#include "game/multiplayer/jak2/wire/packets/airlock_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/boss_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/enemy_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/game_event_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/pedestrian_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/player_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/player_vehicle_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/traffic_authority_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/turret_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/vehicle_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/world_state_packet.h"
#include "game/multiplayer/platform/session/typed_packet_handler.h"

namespace multiplayer::jak2::application {
namespace {

using Handlers = std::vector<std::unique_ptr<platform::PacketHandler>>;
using Ready = std::function<std::optional<bool>()>;
using Relay =
    std::function<std::optional<std::vector<platform::PlayerId>>(const platform::GameplayMessage&,
                                                                 const platform::SessionState&)>;

template <typename Packet, typename Model, typename Domain, typename Produce>
void add_packet_handler(
    Handlers& handlers,
    Packet (*convert)(const Model&),
    Domain& domain,
    void (*canonicalize)(Model&, const platform::MessageOrigin&),
    Produce produce,
    Ready ready,
    Relay relay = [](const auto&, const auto&) { return std::nullopt; }) {
  using Handler = platform::TypedPacketHandler<Model, Packet>;
  handlers.push_back(std::make_unique<Handler>(
      Packet::kPolicy,
      typename Handler::Hooks{
          .to_wire = convert,
          .canonicalize = canonicalize,
          .apply = [&domain](const Model& model,
                             const auto& context) { return domain.apply(model, context); },
          .produce = std::move(produce),
          .ready = std::move(ready),
          .relay = std::move(relay)}));
}

Ready frame_ready(const std::unique_ptr<LocalReplicationFrame>& frame) {
  return [&frame]() -> std::optional<bool> {
    return frame && frame->local_player_id < core::kMaxPlayers ? std::optional{true} : std::nullopt;
  };
}

bool bootstrap_equal(const core::BootstrapState& left, const core::BootstrapState& right) {
  const auto& a = left.world;
  const auto& b = right.world;
  return a.money == b.money && a.gems == b.gems && a.skill == b.skill &&
         a.task_mask == b.task_mask && a.active_task_mask == b.active_task_mask &&
         left.host_task == right.host_task && left.host_continue == right.host_continue &&
         left.host_spawn_position == right.host_spawn_position &&
         left.host_spawn_angle == right.host_spawn_angle &&
         left.host_camera_angle_y == right.host_camera_angle_y &&
         left.synchronized_aid_count == right.synchronized_aid_count &&
         left.synchronized_aids == right.synchronized_aids;
}

float distance_squared(const std::array<float, 3>& position,
                       const std::array<core::PlayerState, core::kMaxPlayers>& players) {
  float nearest = std::numeric_limits<float>::max();
  for (const auto& player : players) {
    if (!player.state_ready || player.spectator_only)
      continue;
    float squared = 0.0f;
    for (size_t axis = 0; axis < position.size(); ++axis) {
      const float delta = position[axis] - player.position[axis];
      squared += delta * delta;
    }
    nearest = std::min(nearest, squared);
  }
  return nearest;
}

template <typename Entity, typename Important, typename Id>
void prioritize_and_limit(std::vector<Entity>& entities,
                          const LocalReplicationFrame& frame,
                          size_t limit,
                          Important important,
                          Id id) {
  if (entities.size() <= limit)
    return;
  std::stable_sort(entities.begin(), entities.end(), [&](const auto& left, const auto& right) {
    if (important(left) != important(right))
      return important(left);
    const float left_distance = distance_squared(left.position, frame.players);
    const float right_distance = distance_squared(right.position, frame.players);
    return left_distance != right_distance ? left_distance < right_distance : id(left) < id(right);
  });
  entities.resize(limit);
}

}  // namespace

std::vector<std::unique_ptr<platform::PacketHandler>> Jak2Adapter::make_packet_handlers() {
  Handlers handlers;
  add_event_handler(handlers);
  add_player_handlers(handlers);
  add_world_handlers(handlers);
  add_entity_handlers(handlers);
  add_traffic_handlers(handlers);
  add_packet_handler<wire::PalaceSquidStatePacket>(
      handlers, wire::to_palace_squid_state_packet, state_.entities(),
      wire::canonicalize_host_state,
      [this](auto& handler, auto& endpoint, uint64_t now_ms) {
        for (auto boss : local_frame_->bosses) {
          if (boss.kind != core::BossState::Kind::PALACE_SQUID)
            continue;
          boss.sample_time_ms = local_frame_->sample_time_ms;
          handler.send(boss, endpoint, now_ms);
        }
      },
      frame_ready(local_frame_));
  add_packet_handler<wire::WidowStatePacket>(
      handlers, wire::to_widow_state_packet, state_.entities(), wire::canonicalize_host_state,
      [this](auto& handler, auto& endpoint, uint64_t now_ms) {
        for (auto boss : local_frame_->bosses) {
          if (boss.kind != core::BossState::Kind::WIDOW)
            continue;
          boss.sample_time_ms = local_frame_->sample_time_ms;
          handler.send(boss, endpoint, now_ms);
        }
      },
      frame_ready(local_frame_));
  add_packet_handler<wire::AirlockStateBatchPacket>(
      handlers, wire::to_packet, state_.entities(), wire::canonicalize_snapshot,
      [this](auto& handler, auto& endpoint, uint64_t now_ms) {
        auto snapshot = local_frame_->airlocks;
        snapshot.source_player_id = endpoint.snapshot().state.local_player_id;
        handler.send(snapshot, endpoint, now_ms, false);
      },
      frame_ready(local_frame_));
  return handlers;
}

void Jak2Adapter::add_player_handlers(Handlers& handlers) {
  const auto ready = frame_ready(local_frame_);
  add_packet_handler<wire::PlayerStatePacket>(
      handlers, wire::to_packet, state_.participants(), wire::canonicalize_player,
      [this](auto& handler, auto& endpoint, uint64_t now_ms) {
        const auto& session = endpoint.snapshot().state;
        auto player = local_frame_->players[session.local_player_id];
        player.player_id = session.local_player_id;
        player.sample_time_ms = local_frame_->sample_time_ms;
        player.activity = core::PlayerActivity::LOBBY;
        if (session.status == platform::SessionStatus::IN_GAME)
          player.activity = core::PlayerActivity::IN_GAME;
        else if (session.status == platform::SessionStatus::GAME_STARTING)
          player.activity = core::PlayerActivity::GAME_STARTING;
        handler.send(player, endpoint, now_ms);
      },
      ready);
  add_packet_handler<wire::PlayerVehicleStatePacket>(
      handlers, wire::to_packet, state_.participants(), wire::canonicalize_player,
      [this](auto& handler, auto& endpoint, uint64_t now_ms) {
        const auto local_id = endpoint.snapshot().state.local_player_id;
        const auto& player = local_frame_->players[local_id];
        if (!local_frame_->player_vehicle || player.spectator_only || player.vehicle_seat != 0 ||
            local_frame_->player_vehicle->vehicle.net_id != player.vehicle_id)
          return;
        auto vehicle = *local_frame_->player_vehicle;
        vehicle.player_id = local_id;
        vehicle.vehicle.sample_time_ms = local_frame_->sample_time_ms;
        handler.send(vehicle, endpoint, now_ms);
      },
      ready);
  add_packet_handler<wire::TurretStatePacket>(
      handlers, wire::to_packet, state_.participants(), wire::canonicalize_player,
      [this](auto& handler, auto& endpoint, uint64_t now_ms) {
        const auto local_id = endpoint.snapshot().state.local_player_id;
        if (!local_frame_->turret || local_frame_->players[local_id].spectator_only)
          return;
        auto turret = *local_frame_->turret;
        turret.player_id = local_id;
        handler.send(turret, endpoint, now_ms);
      },
      ready);
}

void Jak2Adapter::add_world_handlers(Handlers& handlers) {
  add_packet_handler<wire::WorldStatePacket>(
      handlers, wire::to_packet, state_.world(), wire::canonicalize_host_state,
      [this](auto& handler, auto& endpoint, uint64_t now_ms) {
        handler.send(local_frame_->world, endpoint, now_ms);
        auto bootstrap = local_frame_->bootstrap;
        bootstrap.world = local_frame_->world;
        if (!bootstrap_equal(state_.world().bootstrap(), bootstrap))
          state_.apply_bootstrap(bootstrap, 0);
      },
      frame_ready(local_frame_));
}

void Jak2Adapter::add_entity_handlers(Handlers& handlers) {
  add_packet_handler<wire::EnemyStateBatchPacket>(
      handlers, wire::to_packet, state_.entities(), wire::canonicalize_snapshot,
      [this](auto& handler, auto& endpoint, uint64_t now_ms) {
        auto snapshot = local_frame_->enemies;
        if (endpoint.severe_pressure_sustained(now_ms))
          prioritize_and_limit(
              snapshot.enemies, *local_frame_, core::kMaxEnemies / 2,
              [](const auto& enemy) { return enemy.attack_active || enemy.aggro; },
              [](const auto& enemy) { return enemy.actor_id; });
        snapshot.source_player_id = endpoint.snapshot().state.local_player_id;
        snapshot.sample_time_ms = local_frame_->sample_time_ms;
        handler.send(snapshot, endpoint, now_ms);
      },
      frame_ready(local_frame_));
}

void Jak2Adapter::add_traffic_handlers(Handlers& handlers) {
  add_packet_handler<wire::TrafficAuthorityStatePacket>(
      handlers, wire::to_packet, state_.traffic(), wire::canonicalize_host_state,
      [this](auto& handler, auto& endpoint, uint64_t now_ms) {
        if (handler.send(local_frame_->traffic_authority, endpoint, now_ms))
          last_traffic_authority_ = local_frame_->traffic_authority;
      },
      [this]() -> std::optional<bool> {
        if (!local_frame_ || local_frame_->traffic_authority.revision == 0)
          return std::nullopt;
        return !last_traffic_authority_ ||
               last_traffic_authority_->revision != local_frame_->traffic_authority.revision ||
               last_traffic_authority_->assignments != local_frame_->traffic_authority.assignments;
      });
  const auto relay = [this](const auto& message,
                            const auto& session) -> std::optional<std::vector<platform::PlayerId>> {
    std::vector<platform::PlayerId> targets;
    const auto& assignments = state_.traffic().authority().assignments;
    for (core::PlayerId participant = 0;
         participant < session.player_limit && participant < core::kMaxPlayers; ++participant)
      if (participant != message.origin.authenticated_player_id &&
          assignments[participant] == message.origin.authenticated_player_id)
        targets.push_back(participant);
    return targets;
  };
  const auto produce = [this](auto& handler, auto& endpoint, uint64_t now_ms) {
    const auto& session = endpoint.snapshot().state;
    const auto& authority = state_.traffic().authority();
    if (authority.revision == 0 ||
        authority.assignments[session.local_player_id] != session.local_player_id)
      return;
    auto snapshot = handler.policy().id == static_cast<uint8_t>(PacketType::PEDESTRIAN_STATE_BATCH)
                        ? local_frame_->pedestrians
                        : local_frame_->vehicles;
    if (endpoint.severe_pressure_sustained(now_ms)) {
      prioritize_and_limit(
          snapshot.pedestrians, *local_frame_, core::kMaxPedestrians / 2,
          [](const auto&) { return false; },
          [](const auto& pedestrian) { return pedestrian.net_id; });
      prioritize_and_limit(
          snapshot.vehicles, *local_frame_, core::kMaxVehicles / 2,
          [](const auto& vehicle) {
            return std::ranges::any_of(vehicle.rider_player_ids,
                                       [](auto rider) { return rider < core::kMaxPlayers; });
          },
          [](const auto& vehicle) { return vehicle.net_id; });
    }
    snapshot.source_player_id = session.local_player_id;
    snapshot.authority_revision = authority.revision;
    snapshot.level_id =
        core::primary_level_id(local_frame_->players[session.local_player_id].levels);
    snapshot.sample_time_ms = local_frame_->sample_time_ms;
    handler.send(snapshot, endpoint, now_ms);
  };
  const auto ready = frame_ready(local_frame_);
  add_packet_handler<wire::PedestrianStateBatchPacket>(
      handlers, wire::to_pedestrian_state_batch_packet, state_.traffic(),
      wire::canonicalize_snapshot, produce, ready, relay);
  add_packet_handler<wire::VehicleStateBatchPacket>(handlers, wire::to_vehicle_state_batch_packet,
                                                    state_.traffic(), wire::canonicalize_snapshot,
                                                    produce, ready, relay);
}

void Jak2Adapter::add_event_handler(Handlers& handlers) {
  add_packet_handler<wire::GameEventBatchPacket>(
      handlers, wire::to_packet, state_.events(), wire::canonicalize_events,
      [this](auto& handler, auto& endpoint, uint64_t now_ms) {
        auto events = mailbox_.take_outbound_events(kReplicationEventCapacity);
        size_t begin = 0;
        while (begin < events.size()) {
          size_t end = begin;
          size_t wire_size = wire::kGameEventBatchPacketPrefixWireSize;
          while (end < events.size() && end - begin < UINT8_MAX) {
            const size_t entry_size =
                wire::kGameEventRecordPrefixWireSize + events[end].payload_size;
            if (wire_size + entry_size > handler.policy().maximum_payload_bytes)
              break;
            wire_size += entry_size;
            ++end;
          }
          core::GameEventBatch batch = {
              .events = std::vector<core::GameEvent>(events.begin() + begin, events.begin() + end)};
          wire::canonicalize_events(
              batch, {.authenticated_player_id = endpoint.snapshot().state.local_player_id});
          if (end == begin || !handler.send(batch, endpoint, now_ms, false)) {
            mailbox_.restore_outbound_events(
                std::vector<core::GameEvent>(events.begin() + begin, events.end()));
            return;
          }
          begin = end;
        }
      },
      [this]() -> std::optional<bool> {
        return mailbox_.outbound_event_count() != 0 ? std::optional{true} : std::nullopt;
      });
}

}  // namespace multiplayer::jak2::application
