#include "player_sync.h"

#include "common/log/log.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_session.h"

#include <cstring>

namespace {
bool has_host_continue(const LocalPlayerInfoGOAL* local) {
  if (!local) {
    return false;
  }
  for (size_t i = 0; i < sizeof(local->host_continue); ++i) {
    if (local->host_continue[i] != 0) {
      return true;
    }
  }
  return false;
}

bool finite_vehicle_state(const MPVehicleState& state) {
  return mp_float_is_finite(state.x) && mp_float_is_finite(state.y) &&
         mp_float_is_finite(state.z) && mp_float_is_finite(state.quat_x) &&
         mp_float_is_finite(state.quat_y) && mp_float_is_finite(state.quat_z) &&
         mp_float_is_finite(state.quat_w) && mp_float_is_finite(state.lin_vel_x) &&
         mp_float_is_finite(state.lin_vel_y) && mp_float_is_finite(state.lin_vel_z) &&
         mp_float_is_finite(state.ang_vel_x) && mp_float_is_finite(state.ang_vel_y) &&
         mp_float_is_finite(state.ang_vel_z);
}

bool valid_bootstrap_packet(const PacketBootstrap& bootstrap) {
  return mp_float_is_finite(bootstrap.money) && mp_float_is_finite(bootstrap.gems) &&
         mp_float_is_finite(bootstrap.skill) && mp_float_is_finite(bootstrap.x) &&
         mp_float_is_finite(bootstrap.y) && mp_float_is_finite(bootstrap.z) &&
         mp_float_is_finite(bootstrap.tod_ratio) &&
         mp_float_is_finite(bootstrap.weather_cloud) &&
         mp_float_is_finite(bootstrap.weather_fog) &&
         mp_float_is_finite(bootstrap.weather_rain) &&
         mp_float_is_finite(bootstrap.cam_angle_y) && bootstrap.sync_aids_count <= 128 &&
         memchr(bootstrap.host_continue, '\0', sizeof(bootstrap.host_continue)) != nullptr;
}

void apply_bootstrap_to_goal(const PacketBootstrap& bootstrap,
                             LocalPlayerInfoGOAL& local,
                             RemotePlayerInfoGOAL& remote) {
  local.sync_money = bootstrap.money;
  local.sync_gems = bootstrap.gems;
  local.sync_skill = bootstrap.skill;
  remote.x = bootstrap.x;
  remote.y = bootstrap.y;
  remote.z = bootstrap.z;
  local.host_task = bootstrap.host_task;
  local.host_node = bootstrap.host_node;
  memcpy(local.host_continue, bootstrap.host_continue, sizeof(local.host_continue));
  memcpy(local.task_mask, bootstrap.task_mask, sizeof(local.task_mask));
  memcpy(local.active_task_mask, bootstrap.active_task_mask, sizeof(local.active_task_mask));
  local.sync_aids_count = bootstrap.sync_aids_count;
  memcpy(local.sync_aids, bootstrap.sync_aids, sizeof(local.sync_aids));
  local.clock = bootstrap.clock;
  remote.tod_frame = bootstrap.tod_frame;
  remote.tod_ratio = bootstrap.tod_ratio;
  remote.weather_cloud = bootstrap.weather_cloud;
  remote.weather_fog = bootstrap.weather_fog;
  remote.weather_rain = bootstrap.weather_rain;
  if (local.sync_flag <= 1) {
    local.sync_flag = 1;
  }
}

PacketBootstrap make_bootstrap_packet(const LocalPlayerInfoGOAL& local, uint32_t sequence_num) {
  PacketBootstrap bootstrap = {};
  bootstrap.header.type = PacketType::BOOTSTRAP;
  bootstrap.header.sequenceNum = sequence_num;
  bootstrap.money = local.money;
  bootstrap.gems = local.gems;
  bootstrap.skill = local.skill;
  bootstrap.x = local.x;
  bootstrap.y = local.y;
  bootstrap.z = local.z;
  bootstrap.host_task = local.host_task;
  bootstrap.host_node = local.host_node;
  memcpy(bootstrap.host_continue, local.host_continue, sizeof(bootstrap.host_continue));
  memcpy(bootstrap.task_mask, local.task_mask, sizeof(bootstrap.task_mask));
  memcpy(bootstrap.active_task_mask, local.active_task_mask, sizeof(bootstrap.active_task_mask));
  bootstrap.sync_aids_count = mp_clamp_count(local.sync_aids_count, 128);
  memcpy(bootstrap.sync_aids, local.sync_aids, sizeof(bootstrap.sync_aids));
  bootstrap.clock = local.clock;
  bootstrap.tod_frame = local.tod_frame;
  bootstrap.tod_ratio = local.tod_ratio;
  bootstrap.weather_cloud = local.weather_cloud;
  bootstrap.weather_fog = local.weather_fog;
  bootstrap.weather_rain = local.weather_rain;
  return bootstrap;
}
}

void mp_handle_player_state_packet(MultiplayerData& data,
                                   const ENetPacket* packet,
                                   RemotePlayerInfoGOAL* remote,
                                   uint32_t current_time) {
  const auto state = PacketView(packet).as_exact<PacketPlayerState>(PacketType::STATE_UPDATE);
  if (!state) {
    return;
  }
  if (!mp_float_is_finite(state->x) || !mp_float_is_finite(state->y) ||
      !mp_float_is_finite(state->z) || !mp_float_is_finite(state->angle) ||
      !mp_float_is_finite(state->vel_x) || !mp_float_is_finite(state->vel_y) ||
      !mp_float_is_finite(state->vel_z) || !mp_float_is_finite(state->cam_angle_y) ||
      !mp_float_is_finite(state->tod_ratio) || !mp_float_is_finite(state->weather_cloud) ||
      !mp_float_is_finite(state->weather_fog) || !mp_float_is_finite(state->weather_rain) ||
      !mp_float_is_finite(state->money) || !mp_float_is_finite(state->gems) ||
      !mp_float_is_finite(state->skill) || !finite_vehicle_state(state->veh_state)) {
    return;
  }
  if (state->status > static_cast<uint8_t>(MultiplayerStatus::HOST_LEFT) &&
      state->status != static_cast<uint8_t>(MultiplayerStatus::FAILED)) {
    return;
  }

  const uint32_t expected_remote_id = data.local_role == 0 ? 1 : 0;
  if (state->netId != expected_remote_id) {
    return;
  }

  auto& entity = data.remote_entity;
  if (!mp_sequence_is_newer(state->header.sequenceNum, entity.last_sequence_num)) {
    return;
  }

  if (state->netId != data.local_net_id && state->level_hash != 0 &&
      data.last_remote_traffic_level_hash != 0 &&
      state->level_hash != data.last_remote_traffic_level_hash) {
    multiplayer_reset_remote_traffic_buffers(data);
    multiplayer_reset_remote_palace_squid_state(data);
    multiplayer_reset_remote_airlock_state(data);
    lg::info("[Multiplayer] Remote level changed. Cleared traffic sync buffers. old={} new={}",
             data.last_remote_traffic_level_hash, state->level_hash);
  }
  if (state->netId != data.local_net_id && state->level_hash != 0) {
    data.last_remote_traffic_level_hash = state->level_hash;
  }

  entity.status = state->status;
  entity.x = state->x;
  entity.y = state->y;
  entity.z = state->z;
  entity.angle = state->angle;
  entity.vel_x = state->vel_x;
  entity.vel_y = state->vel_y;
  entity.vel_z = state->vel_z;
  entity.send_tick = state->send_tick;
  entity.receive_tick = current_time;
  entity.state_id = state->state_id;
  entity.level_hash = state->level_hash;
  entity.darkjak_stage = state->darkjak_stage;
  entity.clock = state->clock;
  entity.tod_frame = state->tod_frame;
  entity.tod_ratio = state->tod_ratio;
  entity.weather_cloud = state->weather_cloud;
  entity.weather_fog = state->weather_fog;
  entity.weather_rain = state->weather_rain;
  entity.buttons = state->buttons;
  entity.leftx = state->leftx;
  entity.lefty = state->lefty;
  entity.rightx = state->rightx;
  entity.righty = state->righty;
  entity.respawn_flags = state->respawn_flags;
  entity.cam_angle_y = state->cam_angle_y;
  entity.riding_veh_id = state->riding_veh_id;
  entity.riding_seat_index = state->riding_seat_index;
  entity.scene_active = state->scene_active;
  entity.equipped_weapon = state->equipped_weapon;
  entity.turret_active = state->turret_active;
  entity.last_sequence_num = state->header.sequenceNum;
  entity.action_seq = state->action_seq;
  entity.action_state_id = state->action_state_id;
  memcpy(&entity.veh_state, &state->veh_state, sizeof(MPVehicleState));

  if (data.local_role == 0 && state->netId == 1 &&
      state->status == (uint8_t)MultiplayerStatus::IN_GAME && data.pending_bootstrap &&
      data.pending_bootstrap_sent_once) {
    data.pending_bootstrap = false;
    data.pending_bootstrap_sent_once = false;
    lg::info("[MP-Reconnect] Client entered game. Bootstrap acknowledged (sequence={}, status={}).",
             state->header.sequenceNum, state->status);
  }

  if (remote && state->netId == 0) {
    remote->money = state->money;
    remote->gems = state->gems;
    remote->skill = state->skill;
    memcpy(remote->task_mask, state->task_mask, sizeof(remote->task_mask));
    memcpy(remote->active_task_mask, state->active_task_mask, sizeof(remote->active_task_mask));
  }
}

void mp_handle_turret_state_packet(MultiplayerData& data, const ENetPacket* packet) {
  const auto state = PacketView(packet).as_exact<PacketTurretState>(PacketType::TURRET_SYNC);
  if (!state) {
    return;
  }
  if (!mp_float_is_finite(state->roty) || !mp_float_is_finite(state->rotx)) {
    return;
  }

  const uint32_t expected_remote_id = data.local_role == 0 ? 1 : 0;
  if (state->netId != expected_remote_id) {
    return;
  }
  auto& entity = data.remote_entity;
  if (!mp_sequence_is_newer(state->header.sequenceNum, entity.last_turret_sequence_num)) {
    return;
  }
  entity.last_turret_sequence_num = state->header.sequenceNum;
  if (state->turret_aid != 0) {
    entity.turret_roty = state->roty;
    entity.turret_rotx = state->rotx;
  }
}

void mp_handle_bootstrap_packet(const ENetPacket* packet,
                                LocalPlayerInfoGOAL* local,
                                RemotePlayerInfoGOAL* remote) {
  const auto bootstrap = PacketView(packet).as_exact<PacketBootstrap>(PacketType::BOOTSTRAP);
  if (!bootstrap || !local || !remote) {
    return;
  }
  if (!valid_bootstrap_packet(*bootstrap)) {
    return;
  }
  apply_bootstrap_to_goal(*bootstrap, *local, *remote);
}

void mp_send_player_state(MultiplayerData& data, LocalPlayerInfoGOAL* local) {
  if (!local) {
    return;
  }

  PacketPlayerState local_state = {};
  local_state.header.type = PacketType::STATE_UPDATE;
  local_state.header.sequenceNum = ++data.sequence_num;
  local_state.netId = data.local_net_id;
  local_state.status = (uint8_t)data.join_status;
  local_state.x = local->x;
  local_state.y = local->y;
  local_state.z = local->z;
  local_state.angle = local->angle;
  local_state.vel_x = local->velocity[0];
  local_state.vel_y = local->velocity[1];
  local_state.vel_z = local->velocity[2];
  local_state.send_tick = enet_time_get();
  local->send_tick = local_state.send_tick;
  local_state.state_id = local->state_id;
  local_state.level_hash = local->level;
  data.local_traffic_level_hash = local_state.level_hash;
  local_state.darkjak_stage = local->darkjak_stage;
  local_state.clock = local->clock;
  local_state.tod_frame = local->tod_frame;
  local_state.tod_ratio = local->tod_ratio;
  local_state.weather_cloud = local->weather_cloud;
  local_state.weather_fog = local->weather_fog;
  local_state.weather_rain = local->weather_rain;
  local_state.buttons = local->buttons;
  local_state.leftx = local->leftx;
  local_state.lefty = local->lefty;
  local_state.rightx = local->rightx;
  local_state.righty = local->righty;
  local_state.respawn_flags = local->respawn_flags;
  local_state.cam_angle_y = local->cam_angle_y;
  local_state.riding_veh_id = local->riding_veh_id;
  local_state.riding_seat_index = local->riding_seat_index;
  local_state.scene_active = local->scene_active;
  local_state.equipped_weapon = local->equipped_weapon;
  local_state.turret_active = local->turret_active;
  local_state.action_seq = local->action_seq;
  local_state.action_state_id = local->action_state_id;
  local_state.money = local->money;
  local_state.gems = local->gems;
  local_state.skill = local->skill;
  memcpy(local_state.task_mask, local->task_mask, sizeof(local_state.task_mask));
  memcpy(local_state.active_task_mask, local->active_task_mask, sizeof(local_state.active_task_mask));
  memcpy(&local_state.veh_state, &local->veh_state, sizeof(MPVehicleState));
  MultiplayerManager::broadcast(data, 0, local_state, ENET_PACKET_FLAG_UNSEQUENCED);

  if (local->turret_active && local->riding_veh_id != 0) {
    PacketTurretState turret_state = {};
    turret_state.header.type = PacketType::TURRET_SYNC;
    turret_state.header.sequenceNum = ++data.sequence_num;
    turret_state.netId = data.local_net_id;
    turret_state.turret_aid = local->riding_veh_id;
    turret_state.roty = local->turret_roty;
    turret_state.rotx = local->turret_rotx;
    MultiplayerManager::broadcast(data, 0, turret_state, ENET_PACKET_FLAG_UNSEQUENCED);
  }

  if (data.local_role != 0 || !data.pending_bootstrap) {
    return;
  }

  uint32_t current_time = enet_time_get();
  if (!data.host || data.host->connectedPeers == 0) {
    return;
  }
  if (data.join_status != (int)MultiplayerStatus::IN_GAME || !has_host_continue(local)) {
    return;
  }
  if (data.last_bootstrap_send_time != 0 && current_time - data.last_bootstrap_send_time < 500) {
    return;
  }
  data.last_bootstrap_send_time = current_time;

  const auto bootstrap = make_bootstrap_packet(*local, ++data.sequence_num);
  const bool queued = MultiplayerManager::broadcast(data, 1, bootstrap, ENET_PACKET_FLAG_RELIABLE);
  data.pending_bootstrap_sent_once = true;
  lg::info("[MP-Reconnect] Bootstrap {} for client (sequence={}, pending={}, queued_packets={}, queued_bytes={}).",
           queued ? "queued" : "rejected", bootstrap.header.sequenceNum,
           data.pending_bootstrap.load(), data.packet_scheduler.queued_packet_count(),
           data.packet_scheduler.queued_byte_count());
}

void mp_sync_remote_player_to_goal(MultiplayerData& data, RemotePlayerInfoGOAL* remote_goal) {
  if (!remote_goal) {
    return;
  }

  uint32_t other_net_id = (data.local_role == 0) ? 1 : 0;
  if (data.remote_entity.last_sequence_num == 0) {
    remote_goal->status = 0;
    remote_goal->scene_active = 0;
    remote_goal->turret_active = 0;
    remote_goal->respawn_flags = 0;
    remote_goal->respawn_pad = 0;
    remote_goal->turret_roty = 0.0f;
    remote_goal->turret_rotx = 0.0f;
    remote_goal->velocity[0] = 0.0f;
    remote_goal->velocity[1] = 0.0f;
    remote_goal->velocity[2] = 0.0f;
    remote_goal->velocity[3] = 0.0f;
    remote_goal->send_tick = 0;
    remote_goal->receive_tick = 0;
    remote_goal->darkjak_stage = 0;
    remote_goal->riding_veh_id = 0;
    remote_goal->riding_seat_index = 0;
    remote_goal->action_seq = 0;
    remote_goal->action_state_id = 0;
    return;
  }

  const auto& remote_state = data.remote_entity;
  uint64_t age_ms = 0;
  uint32_t current_time = enet_time_get();
  if (remote_state.receive_tick != 0 && current_time >= remote_state.receive_tick) {
    age_ms = current_time - remote_state.receive_tick;
    if (age_ms > 100) {
      age_ms = 100;
    }
  }
  float predict_dt = (float)age_ms * 0.001f;
  remote_goal->x = remote_state.x + remote_state.vel_x * predict_dt;
  remote_goal->y = remote_state.y + remote_state.vel_y * predict_dt;
  remote_goal->z = remote_state.z + remote_state.vel_z * predict_dt;
  remote_goal->angle = remote_state.angle;
  remote_goal->velocity[0] = remote_state.vel_x;
  remote_goal->velocity[1] = remote_state.vel_y;
  remote_goal->velocity[2] = remote_state.vel_z;
  remote_goal->velocity[3] = 0.0f;
  remote_goal->send_tick = remote_state.send_tick;
  remote_goal->receive_tick = remote_state.receive_tick;
  remote_goal->id = other_net_id;
  remote_goal->role = (int32_t)other_net_id;
  remote_goal->state_id = remote_state.state_id;
  remote_goal->level = remote_state.level_hash;
  remote_goal->status = (remote_state.status > 0) ? (int32_t)remote_state.status : 1;
  remote_goal->packet_id = remote_state.last_sequence_num;
  remote_goal->darkjak_stage = remote_state.darkjak_stage;
  remote_goal->clock = remote_state.clock;
  remote_goal->tod_frame = remote_state.tod_frame;
  remote_goal->tod_ratio = remote_state.tod_ratio;
  remote_goal->weather_cloud = remote_state.weather_cloud;
  remote_goal->weather_fog = remote_state.weather_fog;
  remote_goal->weather_rain = remote_state.weather_rain;
  remote_goal->buttons = remote_state.buttons;
  remote_goal->leftx = remote_state.leftx;
  remote_goal->lefty = remote_state.lefty;
  remote_goal->rightx = remote_state.rightx;
  remote_goal->righty = remote_state.righty;
  remote_goal->respawn_flags = remote_state.respawn_flags;
  remote_goal->respawn_pad = 0;
  remote_goal->cam_angle_y = remote_state.cam_angle_y;
  remote_goal->riding_veh_id = remote_state.riding_veh_id;
  remote_goal->riding_seat_index = remote_state.riding_seat_index;
  remote_goal->scene_active = remote_state.scene_active;
  remote_goal->equipped_weapon = remote_state.equipped_weapon;
  remote_goal->turret_active = remote_state.turret_active;
  remote_goal->action_seq = remote_state.action_seq;
  remote_goal->action_state_id = remote_state.action_state_id;
  remote_goal->turret_roty = remote_state.turret_roty;
  remote_goal->turret_rotx = remote_state.turret_rotx;
  memcpy(&remote_goal->veh_state, &remote_state.veh_state, sizeof(MPVehicleState));
}
