#include "game/multiplayer/jak2/bridge/goal_bridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <vector>

#include "common/goal_constants.h"

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kscheme.h"
#include "game/multiplayer/jak2/api/preferences.h"
#include "game/multiplayer/jak2/application/replication_mailbox.h"
#include "game/multiplayer/jak2/bridge/goal_preferences_types.h"
#include "game/multiplayer/jak2/bridge/goal_replication_types.h"
#include "game/multiplayer/jak2/core/validation.h"

namespace multiplayer::jak2::bridge {
namespace {

constexpr size_t kGoalEventCapacity = 64;
constexpr uint32_t kMinGoalPointer = 0x1000;
constexpr uint32_t kMaxBridgeStringLength = 4096;

uint64_t steady_time_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

template <typename T>
T* goal_ptr(const uint32_t address) {
  if (address < kMinGoalPointer || address > static_cast<uint32_t>(EE_MAIN_MEM_SIZE - sizeof(T))) {
    return nullptr;
  }
  return Ptr<T>(address).c();
}

String* goal_string(const uint32_t address) {
  if (address < kMinGoalPointer ||
      address > static_cast<uint32_t>(EE_MAIN_MEM_SIZE - sizeof(String) - 1)) {
    return nullptr;
  }
  auto* string = Ptr<String>(address).c();
  if (!string || string->len > kMaxBridgeStringLength ||
      string->len > static_cast<uint32_t>(EE_MAIN_MEM_SIZE) - address - sizeof(String) - 1) {
    return nullptr;
  }
  return string;
}

const char* goal_string_data(const uint32_t address) {
  auto* string = goal_string(address);
  if (!string)
    return nullptr;
  const char* value = string->data();
  return std::memchr(value, '\0', string->len + 1) ? value : nullptr;
}

core::PlayerIdentity read_identity(const MPReplicationPlayerIdentityGOAL& source) {
  core::PlayerIdentity result = {};
  result.player_id = source.player_id;
  result.character = static_cast<core::PlayerCharacter>(static_cast<uint32_t>(source.character));
  result.identity_ready = source.identity_ready != 0;
  result.state_ready = source.state_ready != 0;
  result.joined = source.joined != 0;
  result.spectator_only = source.spectator_only != 0;
  result.lobby_ready = source.lobby_ready != 0;
  std::ranges::copy_n(reinterpret_cast<const uint8_t*>(source.name), result.name.size(),
                      result.name.begin());
  const auto [colors, strengths] = get_player_appearance_from_goal(source.appearance);
  std::ranges::copy(colors, result.appearance.colors.begin());
  std::ranges::copy(strengths, result.appearance.strengths.begin());
  return result;
}

core::VehicleState read_vehicle(const MPReplicationVehicleStateGOAL& source) {
  core::VehicleState result = {};
  result.net_id = source.net_id;
  result.vehicle_type = source.vehicle_type;
  result.color_index = source.color_index;
  result.state_id = source.state_id;
  result.target_player_id = source.target_player_id;
  result.position = {source.x, source.y, source.z};
  result.quaternion = {source.quat_x, source.quat_y, source.quat_z, source.quat_w};
  result.linear_velocity = {source.lin_vel_x, source.lin_vel_y, source.lin_vel_z};
  result.angular_velocity = {source.ang_vel_x, source.ang_vel_y, source.ang_vel_z};
  result.state_flags = source.state_flags;
  result.hit_points = source.hit_points;
  result.level_id = source.level_id;
  std::ranges::copy(source.rider_player_ids, result.rider_player_ids.begin());
  return result;
}

core::PlayerState read_player(const MPReplicationPlayerGOAL& source) {
  core::PlayerState result = {};
  result.player_id = source.identity.player_id;
  result.activity = static_cast<core::PlayerActivity>(source.activity);
  result.identity_ready = source.identity.identity_ready != 0;
  result.state_ready = source.identity.state_ready != 0;
  result.spectator_only = source.identity.spectator_only != 0;
  result.scene_active = source.action.scene_state != 0;
  result.turret_active = source.vehicle.turret_active != 0;
  for (size_t i = 0; i < core::kPlayerLevelSlotCount; ++i) {
    result.levels[i].level_id = source.transform.levels[i].level_id;
    result.levels[i].flags = source.transform.levels[i].flags;
  }
  result.position = {source.transform.position[0], source.transform.position[1],
                     source.transform.position[2]};
  result.velocity = {source.transform.velocity[0], source.transform.velocity[1],
                     source.transform.velocity[2]};
  result.angle = source.transform.angle;
  result.camera_angle_y = source.input.camera_angle_y;
  result.state_id = source.action.target_state_id;
  result.darkjak_stage = static_cast<uint8_t>(source.action.darkjak_stage);
  result.buttons = source.input.buttons;
  result.leftx = source.input.leftx;
  result.lefty = source.input.lefty;
  result.respawn_flags = source.action.respawn_flags;
  result.equipped_weapon = source.input.equipped_weapon;
  result.action_sequence = source.action.authoritative_sequence;
  result.action_state_id = source.action.action_state_id;
  result.vehicle_id = source.vehicle.vehicle_id;
  result.vehicle_seat = source.vehicle.seat_index;
  result.riding_along_player_id = source.action.riding_along_player_id;
  result.mission_flags = source.mission_flags;
  result.visual_secrets = static_cast<uint8_t>(source.action.visual_secrets);
  return result;
}

GungameTargetRecordGOAL* gungame_targets(const GungameStateGOAL& state, size_t count) {
  if (count > state.capacity || state.targets < kMinGoalPointer ||
      state.targets >= EE_MAIN_MEM_SIZE || state.targets % alignof(GungameTargetRecordGOAL) != 0 ||
      count > (EE_MAIN_MEM_SIZE - state.targets) / sizeof(GungameTargetRecordGOAL))
    return nullptr;
  return Ptr<GungameTargetRecordGOAL>(state.targets).c();
}

bool read_gungame(const GungameStateGOAL& source, core::GungameState& destination) {
  destination = {.run_id = source.run_id,
                 .score = source.score,
                 .elapsed_time = source.elapsed_time,
                 .course_id = source.course_id,
                 .phase = static_cast<core::GungamePhase>(source.phase),
                 .red_intro_step = source.red_intro_step,
                 .yellow_intro_step = source.yellow_intro_step,
                 .end_door = source.end_door,
                 .open_end = source.open_end != 0};
  if (source.count != 0) {
    const auto* targets = gungame_targets(source, source.count);
    if (!targets)
      return false;
    for (size_t index = 0; index < source.count; ++index)
      destination.targets.push_back(
          {.spawn_time = targets[index].spawn_time,
           .state = static_cast<core::GungameTargetState>(targets[index].state)});
  }
  return source.open_end <= 1 && core::valid_gungame_state(destination);
}

void write_gungame(const core::GungameState& source, GungameStateGOAL& destination) {
  auto* targets = gungame_targets(destination, source.targets.size());
  if (source.sequence == 0 || !core::valid_gungame_state(source) ||
      (!source.targets.empty() && !targets)) {
    destination.sequence = 0;
    destination.count = 0;
    return;
  }
  destination.sequence = source.sequence;
  destination.run_id = source.run_id;
  destination.score = source.score;
  destination.elapsed_time = source.elapsed_time;
  destination.course_id = source.course_id;
  destination.phase = static_cast<uint8_t>(source.phase);
  destination.red_intro_step = source.red_intro_step;
  destination.yellow_intro_step = source.yellow_intro_step;
  destination.end_door = source.end_door;
  destination.open_end = source.open_end;
  destination.count = static_cast<uint16_t>(source.targets.size());
  for (size_t index = 0; index < source.targets.size(); ++index) {
    targets[index].spawn_time = source.targets[index].spawn_time;
    targets[index].state = static_cast<uint8_t>(source.targets[index].state);
  }
}

core::WorldState read_world(const MPWorldSyncStateGOAL& source) {
  core::WorldState result = {};
  result.sequence = source.sequence;
  result.money = source.money;
  result.gems = source.gems;
  result.skill = source.skill;
  result.clock = source.clock;
  result.time_of_day_frame = source.time_of_day_frame;
  result.time_of_day_ratio = source.time_of_day_ratio;
  result.weather_cloud = source.weather_cloud;
  result.weather_fog = source.weather_fog;
  result.weather_rain = source.weather_rain;
  result.respawn_delay_seconds = source.respawn_delay_seconds;
  result.player_collision = source.player_collision != 0;
  result.friendly_fire = source.friendly_fire != 0;
  std::ranges::copy(source.task_mask, result.task_mask.begin());
  std::ranges::copy(std::begin(source.active_task_mask), std::end(source.active_task_mask),
                    result.active_task_mask.begin());
  return result;
}

void read_bootstrap(const MPReplicationBootstrapStateGOAL& source, core::BootstrapState& result) {
  result.sequence = source.sequence;
  result.host_task = source.host_task;
  std::ranges::copy(std::begin(source.host_continue), std::end(source.host_continue),
                    result.host_continue.begin());
  result.host_spawn_position = {source.host_spawn_position[0], source.host_spawn_position[1],
                                source.host_spawn_position[2]};
  result.host_spawn_angle = source.host_spawn_angle;
  result.host_camera_angle_y = source.host_camera_angle_y;
  result.synchronized_aid_count = source.synchronized_aid_count;
  std::ranges::copy(std::begin(source.synchronized_aids), std::end(source.synchronized_aids),
                    result.synchronized_aids.begin());
}

core::EnemyState read_enemy(const MPReplicationEnemyStateGOAL& source) {
  core::EnemyState result = {};
  result.actor_id = source.actor_id;
  result.owner_player_id = source.owner_player_id;
  result.focus_player_id = source.focus_player_id < core::kMaxPlayers
                               ? static_cast<core::PlayerId>(source.focus_player_id)
                               : core::kInvalidPlayerId;
  result.state_id = source.state;
  result.hit_points = source.hp;
  result.position = {source.position[0], source.position[1], source.position[2]};
  result.quaternion = {source.quaternion[0], source.quaternion[1], source.quaternion[2],
                       source.quaternion[3]};
  result.attack_active = source.attack_flag != 0;
  result.aggro = source.is_aggro != 0;
  return result;
}

core::PedestrianState read_pedestrian(const MPReplicationPedestrianStateGOAL& source) {
  core::PedestrianState result = {};
  result.net_id = source.net_id;
  result.object_type = source.object_type;
  result.appearance_mask = source.appearance_mask;
  result.state_id = source.state_id;
  result.hit_points = source.hp;
  result.target_player_id = source.target_player_id;
  result.vehicle_net_id = source.vehicle_net_id;
  result.transport_id = source.transport_id;
  result.transport_side = source.transport_side;
  result.state_flags = source.flags;
  result.level_id = source.level_id;
  result.position = {source.position[0], source.position[1], source.position[2]};
  result.quaternion = {source.quaternion[0], source.quaternion[1], source.quaternion[2],
                       source.quaternion[3]};
  return result;
}

core::GameEvent read_event(const MPEventGOAL& source) {
  core::GameEvent result = {};
  result.event_id = source.etype;
  result.source_player_id = source.source_player_id;
  result.payload_size = std::min<uint8_t>(source.payload_size, result.payload.size());
  std::ranges::copy_n(std::begin(source.data), result.payload_size, result.payload.begin());
  return result;
}

core::BossState read_squid(const MPReplicationPalaceSquidStateGOAL& source) {
  core::BossState result = {};
  result.active = source.active;
  result.state_id = source.state_id;
  result.stage = source.stage;
  result.hit_points = source.hit_points;
  result.shield_hit_points = source.shield_hit_points;
  result.target_player_id = source.target_player_id < kMPMaxPlayers
                                ? static_cast<core::PlayerId>(source.target_player_id)
                                : core::kInvalidPlayerId;
  result.draw_force_fade = source.draw_force_fade;
  result.action_sequence = source.action_seq;
  result.position = {source.x, source.y, source.z};
  result.quaternion = {source.quat_x, source.quat_y, source.quat_z, source.quat_w};
  result.root_position = {source.root_x, source.root_y, source.root_z};
  result.root_quaternion = {source.root_quat_x, source.root_quat_y, source.root_quat_z,
                            source.root_quat_w};
  result.trajectory_source = {source.traj_src_x, source.traj_src_y, source.traj_src_z};
  result.trajectory_destination = {source.traj_dest_x, source.traj_dest_y, source.traj_dest_z};
  result.trajectory_duration = source.traj_duration;
  result.trajectory_age = source.traj_age;
  return result;
}

core::BossState read_widow(const MPReplicationWidowStateGOAL& source) {
  core::BossState result = {};
  result.kind = core::BossState::Kind::WIDOW;
  result.active = source.active;
  result.state_id = source.state_id;
  result.position = {source.x, source.y, source.z};
  result.quaternion = {source.quat_x, source.quat_y, source.quat_z, source.quat_w};
  return result;
}

bool capture_local(MPReplicationStateGOAL& state, application::ReplicationMailbox& mailbox) {
  if (!core::valid_index(state.local_player_id, core::kMaxPlayers) ||
      !core::valid_index(state.host_player_id, core::kMaxPlayers) ||
      state.local.enemies.count > core::kMaxEnemies ||
      state.local.traffic.pedestrian_count > kMPReplicationPedestrianCapacity ||
      state.local.traffic.vehicle_count > kMPReplicationVehicleCapacity ||
      state.local.airlocks.count > core::kMaxAirlockStatesPerSnapshot ||
      state.outbound_event_count > kGoalEventCapacity ||
      state.local.bootstrap.synchronized_aid_count > core::kMaxBootstrapAids) {
    return false;
  }
  auto frame = std::make_unique<application::LocalReplicationFrame>();
  frame->sample_time_ms = static_cast<uint32_t>(steady_time_ms());
  frame->local_player_id = state.local_player_id;
  frame->host_player_id = state.host_player_id;
  for (size_t i = 0; i < core::kMaxPlayers; ++i) {
    frame->identities[i] = read_identity(state.local.players[i].identity);
    frame->players[i] = read_player(state.local.players[i]);
  }
  const auto& player = state.local.players[state.local_player_id];
  if (player.vehicle.vehicle_id != 0) {
    frame->player_vehicle = core::PlayerVehicleState{.player_id = state.local_player_id,
                                                     .seat_index = player.vehicle.seat_index,
                                                     .vehicle = read_vehicle(player.vehicle.state)};
  }
  if (player.vehicle.turret_active != 0 && player.vehicle.vehicle_id != 0) {
    frame->turret = core::TurretState{.player_id = state.local_player_id,
                                      .turret_aid = player.vehicle.vehicle_id,
                                      .rotation_y = player.vehicle.turret_roty,
                                      .rotation_x = player.vehicle.turret_rotx};
  }
  frame->world = read_world(state.local.world);
  if (!read_gungame(state.local.gungame, frame->gungame.emplace()))
    frame->gungame.reset();
  read_bootstrap(state.local.bootstrap, frame->bootstrap);
  frame->enemies.source_player_id = state.local_player_id;
  for (uint8_t i = 0; i < state.local.enemies.count; ++i)
    frame->enemies.enemies.push_back(read_enemy(state.local.enemies.enemies[i]));
  frame->pedestrians.kind = core::TrafficSnapshot::Kind::PEDESTRIANS;
  frame->vehicles.kind = core::TrafficSnapshot::Kind::VEHICLES;
  for (uint8_t i = 0; i < state.local.traffic.pedestrian_count; ++i)
    frame->pedestrians.pedestrians.push_back(
        read_pedestrian(state.local.traffic.pedestrians[i].value));
  for (uint8_t i = 0; i < state.local.traffic.vehicle_count; ++i)
    frame->vehicles.vehicles.push_back(read_vehicle(state.local.traffic.vehicles[i]));
  frame->bosses[0] = read_squid(state.local.bosses.squid);
  frame->bosses[1] = read_widow(state.local.bosses.widow);
  frame->airlocks.sequence = state.local.airlocks.sequence;
  frame->airlocks.source_player_id = state.local_player_id;
  for (uint8_t i = 0; i < state.local.airlocks.count; ++i) {
    const auto& source = state.local.airlocks.states[i].value;
    frame->airlocks.states.push_back({.airlock_aid = source.airlock_aid,
                                      .state_id = source.state_id,
                                      .level_id = source.level_id,
                                      .sequence = source.sequence});
  }
  frame->traffic_authority.revision = state.local.authority.revision;
  std::ranges::copy(std::begin(state.local.authority.assignments),
                    std::end(state.local.authority.assignments),
                    frame->traffic_authority.assignments.begin());
  frame->selected_traffic_authority = state.local.authority.selected;
  std::vector<core::GameEvent> events;
  for (uint8_t i = 0; i < state.outbound_event_count; ++i)
    events.push_back(read_event(state.outbound_events[i]));
  if (!events.empty() && !mailbox.push_outbound_events(events)) {
    mailbox.publish_local_frame(std::move(frame));
    return true;
  }
  state.outbound_event_count = 0;
  mailbox.publish_local_frame(std::move(frame));
  return true;
}

void write_identity(const core::PlayerIdentity& source,
                    MPReplicationPlayerIdentityGOAL& destination) {
  destination = {};
  destination.player_id = source.player_id;
  destination.character = static_cast<uint8_t>(source.character);
  destination.identity_ready = source.identity_ready;
  destination.state_ready = source.state_ready;
  destination.joined = source.joined;
  destination.spectator_only = source.spectator_only;
  destination.lobby_ready = source.lobby_ready;
  std::memset(destination.name, 0, sizeof(destination.name));
  std::ranges::copy(source.name.begin(), source.name.end(),
                    reinterpret_cast<uint8_t*>(destination.name));
  std::ranges::copy(source.appearance.colors.begin(), source.appearance.colors.end(),
                    std::begin(destination.appearance.colors));
  std::ranges::copy(source.appearance.strengths.begin(), source.appearance.strengths.end(),
                    std::begin(destination.appearance.strengths));
}

void write_vehicle(const core::VehicleState& source, MPReplicationVehicleStateGOAL& destination) {
  destination = {};
  destination.net_id = source.net_id;
  destination.vehicle_type = source.vehicle_type;
  destination.color_index = source.color_index;
  destination.state_id = source.state_id;
  destination.target_player_id = source.target_player_id;
  destination.x = source.position[0];
  destination.y = source.position[1];
  destination.z = source.position[2];
  destination.quat_x = source.quaternion[0];
  destination.quat_y = source.quaternion[1];
  destination.quat_z = source.quaternion[2];
  destination.quat_w = source.quaternion[3];
  destination.lin_vel_x = source.linear_velocity[0];
  destination.lin_vel_y = source.linear_velocity[1];
  destination.lin_vel_z = source.linear_velocity[2];
  destination.ang_vel_x = source.angular_velocity[0];
  destination.ang_vel_y = source.angular_velocity[1];
  destination.ang_vel_z = source.angular_velocity[2];
  destination.state_flags = source.state_flags;
  destination.hit_points = source.hit_points;
  destination.level_id = source.level_id;
  std::ranges::copy(source.rider_player_ids.begin(), source.rider_player_ids.end(),
                    std::begin(destination.rider_player_ids));
}

void write_player(const core::PlayerState& source,
                  const core::PlayerVehicleState& vehicle,
                  const core::TurretState& turret,
                  const application::PresentationTarget& target,
                  MPReplicationPlayerGOAL& destination) {
  destination.valid = target.valid;
  destination.generation = target.generation;
  destination.activity = static_cast<uint8_t>(source.activity);
  destination.mission_flags = source.mission_flags;
  destination.transform.position[0] = source.position[0];
  destination.transform.position[1] = source.position[1];
  destination.transform.position[2] = source.position[2];
  destination.transform.velocity[0] = source.velocity[0];
  destination.transform.velocity[1] = source.velocity[1];
  destination.transform.velocity[2] = source.velocity[2];
  destination.transform.angle = source.angle;
  for (size_t i = 0; i < core::kPlayerLevelSlotCount; ++i) {
    destination.transform.levels[i].level_id = source.levels[i].level_id;
    destination.transform.levels[i].flags = source.levels[i].flags;
  }
  destination.action.target_state_id = source.state_id;
  destination.action.darkjak_stage = source.darkjak_stage;
  destination.action.authoritative_sequence = source.action_sequence;
  destination.action.action_state_id = source.action_state_id;
  destination.action.scene_state = source.scene_active;
  destination.action.respawn_flags = source.respawn_flags;
  destination.action.riding_along_player_id = source.riding_along_player_id;
  destination.action.visual_secrets = source.visual_secrets;
  destination.input.buttons = source.buttons;
  destination.input.leftx = source.leftx;
  destination.input.lefty = source.lefty;
  destination.input.equipped_weapon = source.equipped_weapon;
  destination.input.camera_angle_y = source.camera_angle_y;
  destination.vehicle.vehicle_id = source.vehicle_id;
  destination.vehicle.seat_index = source.vehicle_seat;
  destination.vehicle.turret_active = source.turret_active;
  if (vehicle.player_id == source.player_id)
    write_vehicle(vehicle.vehicle, destination.vehicle.state);
  if (turret.player_id == source.player_id) {
    destination.vehicle.turret_roty = turret.rotation_y;
    destination.vehicle.turret_rotx = turret.rotation_x;
  }
  std::ranges::copy(target.position.begin(), target.position.end(),
                    std::begin(destination.presentation_position));
  std::ranges::copy(target.quaternion.begin(), target.quaternion.end(),
                    std::begin(destination.presentation_quaternion));
  std::ranges::copy(target.velocity.begin(), target.velocity.end(),
                    std::begin(destination.presentation_velocity));
}

void write_world(const core::WorldState& source, MPWorldSyncStateGOAL& destination) {
  destination = {};
  destination.sequence = source.sequence;
  destination.money = source.money;
  destination.gems = source.gems;
  destination.skill = source.skill;
  destination.clock = source.clock;
  destination.time_of_day_frame = source.time_of_day_frame;
  destination.time_of_day_ratio = source.time_of_day_ratio;
  destination.weather_cloud = source.weather_cloud;
  destination.weather_fog = source.weather_fog;
  destination.weather_rain = source.weather_rain;
  destination.respawn_delay_seconds = source.respawn_delay_seconds;
  destination.player_collision = source.player_collision;
  destination.friendly_fire = source.friendly_fire;
  std::ranges::copy(source.task_mask.begin(), source.task_mask.end(),
                    std::begin(destination.task_mask));
  std::ranges::copy(source.active_task_mask.begin(), source.active_task_mask.end(),
                    std::begin(destination.active_task_mask));
}

void write_bootstrap(const core::BootstrapState& source,
                     MPReplicationBootstrapStateGOAL& destination) {
  destination = {};
  destination.sequence = source.sequence;
  destination.host_task = source.host_task;
  std::ranges::copy(source.host_continue.begin(), source.host_continue.end(),
                    std::begin(destination.host_continue));
  std::ranges::copy(source.host_spawn_position.begin(), source.host_spawn_position.end(),
                    std::begin(destination.host_spawn_position));
  destination.host_spawn_angle = source.host_spawn_angle;
  destination.host_camera_angle_y = source.host_camera_angle_y;
  destination.synchronized_aid_count = static_cast<uint16_t>(
      std::min<size_t>(source.synchronized_aid_count, core::kMaxBootstrapAids));
  std::ranges::copy(source.synchronized_aids.begin(), source.synchronized_aids.end(),
                    std::begin(destination.synchronized_aids));
}

void write_enemy(const core::EnemyState& source,
                 const application::PresentationTarget& target,
                 MPReplicationEnemyStateGOAL& destination) {
  destination = {};
  destination.actor_id = source.actor_id;
  destination.owner_player_id = source.owner_player_id;
  destination.focus_player_id = source.focus_player_id;
  destination.state = source.state_id;
  destination.hp = source.hit_points;
  const auto& position = target.valid ? target.position : source.position;
  const auto& quaternion = target.valid ? target.quaternion : source.quaternion;
  std::ranges::copy(position.begin(), position.end(), std::begin(destination.position));
  std::ranges::copy(quaternion.begin(), quaternion.end(), std::begin(destination.quaternion));
  std::ranges::copy(target.velocity.begin(), target.velocity.end(),
                    std::begin(destination.presentation_velocity));
  destination.attack_flag = source.attack_active;
  destination.is_aggro = source.aggro;
}

void write_pedestrian(const core::PedestrianState& source,
                      const application::PresentationTarget& target,
                      MPReplicationPedestrianStateGOAL& destination) {
  destination = {};
  destination.net_id = source.net_id;
  destination.object_type = source.object_type;
  destination.appearance_mask = source.appearance_mask;
  destination.state_id = source.state_id;
  destination.hp = source.hit_points;
  destination.target_player_id = source.target_player_id;
  destination.vehicle_net_id = source.vehicle_net_id;
  destination.transport_id = source.transport_id;
  destination.transport_side = source.transport_side;
  destination.flags = source.state_flags;
  destination.level_id = source.level_id;
  if (target.valid) {
    const float speed = std::hypot(target.velocity[0], target.velocity[2]);
    if (std::isfinite(speed)) {
      destination.travel_speed = static_cast<uint8_t>(
          std::lround(std::clamp(speed / kPedestrianTravelSpeedResolution, 0.0f, 255.0f)));
    }
  }
  const auto& position = target.valid ? target.position : source.position;
  const auto& quaternion = target.valid ? target.quaternion : source.quaternion;
  std::ranges::copy(position.begin(), position.end(), std::begin(destination.position));
  std::ranges::copy(quaternion.begin(), quaternion.end(), std::begin(destination.quaternion));
}

void write_squid(const core::BossState& source,
                 const application::PresentationTarget& target,
                 MPReplicationPalaceSquidStateGOAL& destination) {
  destination = {};
  destination.active = source.active;
  destination.state_id = source.state_id;
  destination.stage = source.stage;
  destination.hit_points = source.hit_points;
  destination.shield_hit_points = source.shield_hit_points;
  destination.target_player_id = source.target_player_id;
  destination.draw_force_fade = source.draw_force_fade;
  destination.action_seq = source.action_sequence;
  destination.x = source.position[0];
  destination.y = source.position[1];
  destination.z = source.position[2];
  destination.quat_x = source.quaternion[0];
  destination.quat_y = source.quaternion[1];
  destination.quat_z = source.quaternion[2];
  destination.quat_w = source.quaternion[3];
  const auto& root_position = target.valid ? target.position : source.root_position;
  const auto& root_quaternion = target.valid ? target.quaternion : source.root_quaternion;
  destination.root_x = root_position[0];
  destination.root_y = root_position[1];
  destination.root_z = root_position[2];
  destination.root_quat_x = root_quaternion[0];
  destination.root_quat_y = root_quaternion[1];
  destination.root_quat_z = root_quaternion[2];
  destination.root_quat_w = root_quaternion[3];
  destination.traj_src_x = source.trajectory_source[0];
  destination.traj_src_y = source.trajectory_source[1];
  destination.traj_src_z = source.trajectory_source[2];
  destination.traj_dest_x = source.trajectory_destination[0];
  destination.traj_dest_y = source.trajectory_destination[1];
  destination.traj_dest_z = source.trajectory_destination[2];
  destination.traj_duration = source.trajectory_duration;
  destination.traj_age = source.trajectory_age;
}

void write_widow(const core::BossState& source,
                 const application::PresentationTarget& target,
                 MPReplicationWidowStateGOAL& destination) {
  destination = {};
  destination.active = source.active;
  destination.state_id = source.state_id;
  const auto& position = target.valid ? target.position : source.position;
  const auto& quaternion = target.valid ? target.quaternion : source.quaternion;
  destination.x = position[0];
  destination.y = position[1];
  destination.z = position[2];
  destination.quat_x = quaternion[0];
  destination.quat_y = quaternion[1];
  destination.quat_z = quaternion[2];
  destination.quat_w = quaternion[3];
}

void write_event(const core::GameEvent& source, MPEventGOAL& destination) {
  destination = {};
  destination.etype = source.event_id;
  destination.source_player_id = source.source_player_id;
  destination.payload_size = std::min<uint8_t>(source.payload_size, sizeof(destination.data));
  std::ranges::copy_n(source.payload.begin(), destination.payload_size,
                      std::begin(destination.data));
}

bool publish_remote(MPReplicationStateGOAL& state, application::ReplicationMailbox& mailbox) {
  if (auto frame = mailbox.take_remote_frame()) {
    const auto inbound_count =
        std::min<size_t>(state.inbound_event_count, kMPReplicationEventCapacity);
    const auto end = std::remove_if(
        std::begin(state.inbound_events), std::begin(state.inbound_events) + inbound_count,
        [&](const MPEventGOAL& event) {
          return event.source_player_id >= core::kMaxPlayers ||
                 state.remote.players[event.source_player_id].lifecycle_generation !=
                     frame->player_lifecycles[event.source_player_id];
        });
    state.inbound_event_count = static_cast<uint8_t>(end - std::begin(state.inbound_events));
    static_assert(std::is_trivially_copyable_v<MPReplicationFrameGOAL>);
    const auto gungame_buffer = state.remote.gungame;
    std::memset(&state.remote, 0, sizeof(state.remote));
    state.remote.gungame.targets = gungame_buffer.targets;
    state.remote.gungame.capacity = gungame_buffer.capacity;
    write_gungame(frame->gungame, state.remote.gungame);
    state.remote.generation = frame->generation;
    state.remote.identity_generation = frame->generation;
    for (size_t i = 0; i < core::kMaxPlayers; ++i) {
      write_identity(frame->identities[i], state.remote.players[i].identity);
      auto player_vehicle = frame->player_vehicles[i];
      if (const auto& vehicle_target = frame->player_vehicle_targets[i]; vehicle_target.valid) {
        player_vehicle.vehicle.position = vehicle_target.position;
        player_vehicle.vehicle.quaternion = vehicle_target.quaternion;
        player_vehicle.vehicle.linear_velocity = vehicle_target.velocity;
      }
      write_player(frame->players[i], player_vehicle, frame->turrets[i], frame->player_targets[i],
                   state.remote.players[i]);
      state.remote.players[i].lifecycle_generation = frame->player_lifecycles[i];
    }
    state.remote.world_valid = frame->world.sequence != 0;
    state.remote.world_generation = frame->generation;
    write_world(frame->world, state.remote.world);
    state.remote.bootstrap_valid = frame->bootstrap.sequence != 0;
    state.remote.bootstrap_generation = frame->generation;
    write_bootstrap(frame->bootstrap, state.remote.bootstrap);
    state.remote.enemies.valid = frame->enemies.sequence != 0;
    state.remote.enemies.generation = frame->enemy_generation;
    uint16_t enemy_count = 0;
    for (size_t i = 0;
         i < frame->enemies.enemies.size() && enemy_count < kMPReplicationEnemyCapacity; ++i) {
      const auto& enemy = frame->enemies.enemies[i];
      if (enemy.owner_player_id == state.local_player_id) {
        continue;
      }
      const auto target = i < frame->enemy_targets.size() ? frame->enemy_targets[i]
                                                          : application::PresentationTarget{};
      write_enemy(enemy, target, state.remote.enemies.enemies[enemy_count++]);
    }
    state.remote.enemies.count = enemy_count;
    state.remote.traffic.valid = frame->selected_traffic.sequence != 0;
    state.remote.traffic.generation = frame->traffic_generation;
    state.remote.traffic.pedestrian_count = static_cast<uint8_t>(std::min<size_t>(
        frame->selected_traffic.pedestrians.size(), kMPReplicationPedestrianCapacity));
    state.remote.traffic.vehicle_count = static_cast<uint8_t>(
        std::min<size_t>(frame->selected_traffic.vehicles.size(), kMPReplicationVehicleCapacity));
    for (uint8_t i = 0; i < state.remote.traffic.pedestrian_count; ++i) {
      const auto target = i < frame->pedestrian_targets.size() ? frame->pedestrian_targets[i]
                                                               : application::PresentationTarget{};
      write_pedestrian(frame->selected_traffic.pedestrians[i], target,
                       state.remote.traffic.pedestrians[i].value);
    }
    for (uint8_t i = 0; i < state.remote.traffic.vehicle_count; ++i) {
      auto vehicle = frame->selected_traffic.vehicles[i];
      const auto target = i < frame->ambient_vehicle_targets.size()
                              ? frame->ambient_vehicle_targets[i]
                              : application::PresentationTarget{};
      if (target.valid) {
        vehicle.position = target.position;
        vehicle.quaternion = target.quaternion;
        vehicle.linear_velocity = target.velocity;
      }
      write_vehicle(vehicle, state.remote.traffic.vehicles[i]);
    }
    state.remote.bosses.generation = frame->generation;
    state.remote.bosses.squid_valid = frame->bosses[0].sequence != 0;
    state.remote.bosses.widow_valid = frame->bosses[1].sequence != 0;
    write_squid(frame->bosses[0], frame->boss_targets[0], state.remote.bosses.squid);
    write_widow(frame->bosses[1], frame->boss_targets[1], state.remote.bosses.widow);
    state.remote.airlocks.valid = 0;
    state.remote.airlocks.generation = frame->generation;
    state.remote.airlocks.sequence = 0;
    state.remote.airlocks.count = 0;
    for (auto& [value] : state.remote.airlocks.states)
      value = {};
    size_t airlock_index = 0;
    for (const auto& snapshot : frame->airlocks) {
      state.remote.airlocks.valid |= snapshot.sequence != 0;
      state.remote.airlocks.sequence = std::max(state.remote.airlocks.sequence, snapshot.sequence);
      if (!core::valid_index(snapshot.source_player_id, core::kMaxPlayers))
        continue;
      const auto state_count = std::min(snapshot.states.size(), core::kMaxAirlockStatesPerSnapshot);
      for (size_t state_index = 0; state_index < state_count; ++state_index) {
        if (airlock_index >= kMPReplicationAirlockCapacity)
          break;
        const auto& [airlock_aid, state_id, level_id, sequence] = snapshot.states[state_index];
        auto& destination = state.remote.airlocks.states[airlock_index++].value;
        destination.airlock_aid = airlock_aid;
        destination.sequence = sequence;
        destination.level_id = static_cast<uint16_t>(level_id);
        destination.state_id = static_cast<uint8_t>(state_id);
        destination.source_player_id = snapshot.source_player_id;
      }
    }
    state.remote.airlocks.count = static_cast<uint8_t>(airlock_index);
    state.remote.authority.valid = 1;
    state.remote.authority.generation = frame->generation;
    state.remote.authority.revision = frame->traffic_authority.revision;
    state.remote.authority.selected = frame->selected_traffic_authority;
    std::ranges::copy(frame->traffic_authority.assignments.begin(),
                      frame->traffic_authority.assignments.end(),
                      std::begin(state.remote.authority.assignments));
  }
  if (state.inbound_event_count > kGoalEventCapacity)
    return false;
  const auto events = mailbox.take_inbound_events(kGoalEventCapacity - state.inbound_event_count);
  const auto initial = state.inbound_event_count;
  for (size_t i = 0; i < events.size(); ++i)
    write_event(events[i], state.inbound_events[initial + i]);
  state.inbound_event_count = static_cast<uint8_t>(initial + events.size());
  return true;
}

}  // namespace

bool exchange_state(const uint32_t state_address, application::ReplicationMailbox& mailbox) {
  auto* state = goal_ptr<MPReplicationStateGOAL>(state_address);
  if (!state || state->abi_size != sizeof(MPReplicationStateGOAL))
    return false;
  const bool local_captured = capture_local(*state, mailbox);
  const bool remote_published = publish_remote(*state, mailbox);
  return local_captured && remote_published;
}

bool read_string(const uint32_t address, std::string& value) {
  const char* source = goal_string_data(address);
  if (!source)
    return false;
  value.assign(source);
  return true;
}

bool read_preferences(const uint32_t address, MultiplayerPreferences& preferences) {
  const auto* source = goal_ptr<MultiplayerPreferencesGOAL>(address);
  MultiplayerPreferences parsed;
  if (!source || source->automatic_port_mapping > 1 || source->player_collision > 1 ||
      source->friendly_fire > 1 || source->player_map_marker > 1 ||
      source->nametag_visibility > static_cast<uint8_t>(PlayerNametagVisibility::OFF) ||
      !read_string(source->player_name, parsed.player_name) ||
      !read_string(source->room_code, parsed.room_code)) {
    return false;
  }
  parsed.player_appearance = get_player_appearance_from_goal(source->appearance);
  parsed.network_port = source->network_port;
  parsed.respawn_delay_seconds = source->respawn_delay_seconds;
  parsed.session_player_limit = source->session_player_limit;
  parsed.preferred_character = static_cast<PlayerCharacter>(source->preferred_character);
  parsed.automatic_port_mapping = source->automatic_port_mapping != 0;
  parsed.player_collision = source->player_collision != 0;
  parsed.friendly_fire = source->friendly_fire != 0;
  parsed.nametag_visibility = static_cast<PlayerNametagVisibility>(source->nametag_visibility);
  parsed.player_map_marker = source->player_map_marker != 0;
  preferences = std::move(parsed);
  return true;
}

bool write_preferences(const uint32_t address, const MultiplayerPreferences& preferences) {
  auto* destination = goal_ptr<MultiplayerPreferencesGOAL>(address);
  if (!destination)
    return false;
  auto* name = goal_string(destination->player_name);
  auto* room_code = goal_string(destination->room_code);
  if (!name || !room_code || preferences.player_name.size() > name->len ||
      preferences.room_code.size() > room_code->len) {
    return false;
  }
  std::memset(name->data(), 0, name->len + 1);
  std::memcpy(name->data(), preferences.player_name.data(), preferences.player_name.size());
  std::memset(room_code->data(), 0, room_code->len + 1);
  std::memcpy(room_code->data(), preferences.room_code.data(), preferences.room_code.size());
  copy_player_appearance_to_goal(preferences.player_appearance, destination->appearance);
  destination->network_port = preferences.network_port;
  destination->respawn_delay_seconds = preferences.respawn_delay_seconds;
  destination->session_player_limit = preferences.session_player_limit;
  destination->preferred_character = static_cast<uint8_t>(preferences.preferred_character);
  destination->automatic_port_mapping = preferences.automatic_port_mapping;
  destination->player_collision = preferences.player_collision;
  destination->friendly_fire = preferences.friendly_fire;
  destination->nametag_visibility = static_cast<uint8_t>(preferences.nametag_visibility);
  destination->player_map_marker = preferences.player_map_marker;
  return true;
}

bool read_appearance(const uint32_t address, MPPlayerAppearance& appearance) {
  const auto* source = goal_ptr<MPPlayerAppearanceGOAL>(address);
  if (!source)
    return false;
  appearance = get_player_appearance_from_goal(*source);
  return true;
}

}  // namespace multiplayer::jak2::bridge
