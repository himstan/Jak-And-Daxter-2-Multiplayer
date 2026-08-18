#include "player_sync.h"

#include <algorithm>
#include <cstring>

#include "common/log/log.h"

#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_peer_registry.h"
#include "game/multiplayer/multiplayer_session.h"

namespace {
bool valid_player_name(const char* name) {
  if (!name) {
    return false;
  }
  const auto* terminator = static_cast<const char*>(memchr(name, '\0', kMultiplayerPlayerNameSize));
  if (!terminator) {
    return false;
  }
  for (const char* it = name; it != terminator; ++it) {
    const auto value = static_cast<unsigned char>(*it);
    const bool digit = value >= '0' && value <= '9';
    const bool uppercase = value >= 'A' && value <= 'Z';
    const bool lowercase = value >= 'a' && value <= 'z';
    if (!digit && !uppercase && !lowercase) {
      return false;
    }
  }
  return true;
}

MPPlayerRecordGOAL* joined_record(MPPlayerControllerGOAL* controller, uint32_t player_id) {
  if (!controller || !mp_valid_player_id(player_id)) {
    return nullptr;
  }
  auto& record = controller->records[player_id];
  if (!record.identity.joined || record.identity.player_id != player_id) {
    return nullptr;
  }
  return &record;
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

bool valid_player_state(const PacketPlayerState& state) {
  return mp_valid_player_id(state.player_id) &&
         state.spectator_only <= 1 &&
         (state.riding_along_player_id == kMPInvalidPlayerId ||
          (mp_valid_player_id(state.riding_along_player_id) &&
           state.riding_along_player_id != state.player_id)) &&
         mp_float_is_finite(state.x) &&
         mp_float_is_finite(state.y) && mp_float_is_finite(state.z) &&
         mp_float_is_finite(state.angle) && mp_float_is_finite(state.vel_x) &&
         mp_float_is_finite(state.vel_y) && mp_float_is_finite(state.vel_z) &&
         mp_float_is_finite(state.cam_angle_y) && finite_vehicle_state(state.veh_state);
}

bool valid_world_state(const PacketWorldState& state) {
  return mp_valid_player_id(state.player_id) && mp_float_is_finite(state.time_of_day_ratio) &&
         mp_float_is_finite(state.weather_cloud) && mp_float_is_finite(state.weather_fog) &&
         mp_float_is_finite(state.weather_rain);
}

bool valid_bootstrap_packet(const PacketBootstrap& packet) {
  return mp_float_is_finite(packet.money) && mp_float_is_finite(packet.gems) &&
         mp_float_is_finite(packet.skill) && mp_float_is_finite(packet.x) &&
         mp_float_is_finite(packet.y) && mp_float_is_finite(packet.z) &&
         mp_float_is_finite(packet.tod_ratio) && mp_float_is_finite(packet.weather_cloud) &&
         mp_float_is_finite(packet.weather_fog) && mp_float_is_finite(packet.weather_rain) &&
         mp_float_is_finite(packet.cam_angle_y) && packet.sync_aids_count <= 128 &&
         memchr(packet.host_continue, '\0', sizeof(packet.host_continue)) != nullptr;
}

bool has_host_continue(const MPBootstrapSyncStateGOAL* bootstrap) {
  if (!bootstrap) {
    return false;
  }
  return std::any_of(std::begin(bootstrap->host_continue), std::end(bootstrap->host_continue),
                     [](uint8_t value) { return value != 0; });
}

PacketJoin make_join_packet(const MPPlayerRecordGOAL& local, uint32_t sequence) {
  PacketJoin packet = {};
  packet.header.type = PacketType::EVENT_JOIN;
  packet.header.sequenceNum = sequence;
  packet.player_id = local.identity.player_id;
  packet.character = local.identity.character;
  memcpy(packet.player_name, local.identity.name, sizeof(packet.player_name));
  return packet;
}

bool valid_join_record(const MPPlayerRecordGOAL& local) {
  return mp_valid_player_id(local.identity.player_id) && local.identity.identity_ready &&
         local.identity.joined && mp_valid_player_character(local.identity.character) &&
         valid_player_name(local.identity.name);
}

bool send_local_join_packets(MultiplayerData& data, const MPPlayerRecordGOAL& local) {
  if (!valid_join_record(local)) {
    return false;
  }
  if (data.session_role == 1) {
    if (data.local_join_identity_sent || !data.security.authenticated()) {
      return data.local_join_identity_sent;
    }
    const auto packet = make_join_packet(local, ++data.sequence_num);
    const bool queued = MultiplayerManager::broadcast(
        data, static_cast<int>(MultiplayerChannel::CONTROL), packet, ENET_PACKET_FLAG_RELIABLE);
    if (queued) {
      data.local_join_identity_sent = true;
    }
    return queued;
  }
  if (data.session_role != 0) {
    return false;
  }
  bool sent = false;
  for (auto& session : data.host_peer_sessions) {
    if (!session.authenticated || session.local_identity_sent || !session.peer) {
      continue;
    }
    const auto packet = make_join_packet(local, ++data.sequence_num);
    if (MultiplayerManager::send_to_peer(data, session.peer,
                                         static_cast<int>(MultiplayerChannel::CONTROL), packet,
                                         ENET_PACKET_FLAG_RELIABLE)) {
      session.local_identity_sent = true;
      sent = true;
    }
  }
  return sent;
}

PacketPlayerState make_cached_state_packet(const CachedPlayerState& cached, uint32_t sequence) {
  PacketPlayerState state = {};
  state.header = {PacketType::STATE_UPDATE, sequence};
  state.player_id = cached.player_id;
  state.status = cached.status;
  state.x = cached.x;
  state.y = cached.y;
  state.z = cached.z;
  state.angle = cached.angle;
  state.vel_x = cached.vel_x;
  state.vel_y = cached.vel_y;
  state.vel_z = cached.vel_z;
  state.send_tick = cached.send_tick;
  state.state_id = cached.state_id;
  state.level_hash = cached.level_hash;
  state.darkjak_stage = cached.darkjak_stage;
  state.buttons = cached.buttons;
  state.leftx = cached.leftx;
  state.lefty = cached.lefty;
  state.rightx = cached.rightx;
  state.righty = cached.righty;
  state.respawn_flags = cached.respawn_flags;
  state.spectator_only = cached.spectator_only;
  state.cam_angle_y = cached.cam_angle_y;
  state.riding_veh_id = cached.riding_veh_id;
  state.riding_seat_index = cached.riding_seat_index;
  state.scene_active = cached.scene_active;
  state.equipped_weapon = cached.equipped_weapon;
  state.turret_active = cached.turret_active;
  state.action_seq = cached.action_seq;
  state.action_state_id = cached.action_state_id;
  state.riding_along_player_id = cached.riding_along_player_id;
  state.mission_flags = cached.mission_flags;
  memcpy(&state.veh_state, &cached.veh_state, sizeof(state.veh_state));
  return state;
}

bool valid_local_identity_for_send(const MultiplayerData& data, const MPPlayerRecordGOAL& local) {
  return mp_valid_player_id(data.local_player_id) &&
         local.identity.player_id == data.local_player_id && valid_join_record(local);
}

bool send_join_packet(MultiplayerData& data, const MPPlayerRecordGOAL& local) {
  if (!valid_local_identity_for_send(data, local) ||
      !mp_valid_player_id(local.identity.player_id) ||
      !mp_valid_player_character(local.identity.character) ||
      !valid_player_name(local.identity.name)) {
    return false;
  }
  return send_local_join_packets(data, local);
}

PacketBootstrap make_bootstrap_packet(const MPPlayerRecordGOAL& local,
                                      const MPWorldSyncStateGOAL& world,
                                      const MPBootstrapSyncStateGOAL& bootstrap,
                                      uint32_t sequence) {
  PacketBootstrap packet = {};
  packet.header.type = PacketType::BOOTSTRAP;
  packet.header.sequenceNum = sequence;
  packet.money = world.money;
  packet.gems = world.gems;
  packet.skill = world.skill;
  packet.x = bootstrap.host_spawn_position[0];
  packet.y = bootstrap.host_spawn_position[1];
  packet.z = bootstrap.host_spawn_position[2];
  if (packet.x == 0.0f && packet.y == 0.0f && packet.z == 0.0f) {
    packet.x = local.transform.position[0];
    packet.y = local.transform.position[1];
    packet.z = local.transform.position[2];
  }
  packet.host_task = bootstrap.host_task;
  memcpy(packet.host_continue, bootstrap.host_continue, sizeof(packet.host_continue));
  memcpy(packet.task_mask, world.task_mask, sizeof(packet.task_mask));
  memcpy(packet.active_task_mask, world.active_task_mask, sizeof(packet.active_task_mask));
  packet.sync_aids_count = mp_clamp_count(bootstrap.synchronized_aid_count, 128);
  memcpy(packet.sync_aids, bootstrap.synchronized_aids, sizeof(packet.sync_aids));
  packet.clock = world.clock;
  packet.tod_frame = world.time_of_day_frame;
  packet.tod_ratio = world.time_of_day_ratio;
  packet.weather_cloud = world.weather_cloud;
  packet.weather_fog = world.weather_fog;
  packet.weather_rain = world.weather_rain;
  packet.cam_angle_y = bootstrap.host_camera_angle_y;
  return packet;
}

void send_world_state(MultiplayerData& data,
                      uint32_t local_player_id,
                      const MPWorldSyncStateGOAL& world) {
  if (data.session_role != 0 || !mp_valid_player_id(local_player_id)) {
    return;
  }
  PacketWorldState packet = {};
  packet.header.type = PacketType::WORLD_STATE;
  packet.header.sequenceNum = ++data.sequence_num;
  packet.player_id = local_player_id;
  packet.clock = world.clock;
  packet.time_of_day_frame = world.time_of_day_frame;
  packet.time_of_day_ratio = world.time_of_day_ratio;
  packet.weather_cloud = world.weather_cloud;
  packet.weather_fog = world.weather_fog;
  packet.weather_rain = world.weather_rain;
  memcpy(packet.task_mask, world.task_mask, sizeof(packet.task_mask));
  memcpy(packet.active_task_mask, world.active_task_mask, sizeof(packet.active_task_mask));
  MultiplayerManager::broadcast(data, static_cast<int>(MultiplayerChannel::STATE), packet,
                                ENET_PACKET_FLAG_UNSEQUENCED);
}

void copy_cached_state_to_record(const CachedPlayerState& cached, MPPlayerRecordGOAL& record) {
  if (!cached.identity_ready || !cached.state_ready) {
    return;
  }
  record.connection.status = cached.status > 0 ? cached.status : 1;
  record.connection.latest_state_sequence = cached.last_sequence_num;
  record.connection.latest_turret_sequence = cached.last_turret_sequence_num;
  record.connection.connected = 1;
  record.connection.state_cached = 1;
  record.identity.state_ready = 1;
  record.identity.spectator_only = cached.spectator_only;
  record.transform.position[0] = cached.x;
  record.transform.position[1] = cached.y;
  record.transform.position[2] = cached.z;
  record.transform.position[3] = 1.0f;
  record.transform.velocity[0] = cached.vel_x;
  record.transform.velocity[1] = cached.vel_y;
  record.transform.velocity[2] = cached.vel_z;
  record.transform.velocity[3] = 0.0f;
  record.transform.angle = cached.angle;
  record.transform.level = cached.level_hash;
  record.action.target_state_id = cached.state_id;
  record.action.darkjak_stage = cached.darkjak_stage;
  record.action.authoritative_sequence = cached.action_seq;
  record.action.action_state_id = cached.action_state_id;
  record.action.riding_along_player_id = cached.riding_along_player_id;
  record.action.scene_state = cached.scene_active;
  record.action.respawn_flags = cached.respawn_flags;
  record.input.buttons = cached.buttons;
  record.input.leftx = cached.leftx;
  record.input.lefty = cached.lefty;
  record.input.rightx = cached.rightx;
  record.input.righty = cached.righty;
  record.input.camera_angle_y = cached.cam_angle_y;
  record.input.equipped_weapon = cached.equipped_weapon;
  record.vehicle.vehicle_id = cached.riding_veh_id;
  record.vehicle.seat_index = cached.riding_seat_index;
  record.vehicle.turret_active = cached.turret_active;
  record.vehicle.turret_roty = cached.turret_roty;
  record.vehicle.turret_rotx = cached.turret_rotx;
  record.runtime.mission_flags = cached.mission_flags;
  memcpy(&record.vehicle.state, &cached.veh_state, sizeof(record.vehicle.state));
}
}  // namespace

bool mp_valid_player_id(uint32_t player_id) {
  return player_id < kMPMaxPlayers;
}

bool mp_player_id_allowed_from_sender(const MultiplayerData& data,
                                      uint32_t sender_player_id,
                                      uint32_t claimed_player_id) {
  if (!mp_valid_player_id(sender_player_id) || !mp_valid_player_id(claimed_player_id) ||
      claimed_player_id == data.local_player_id) {
    return false;
  }
  return data.session_role == 0 ? claimed_player_id == sender_player_id : true;
}

void mp_clear_player_slot(MultiplayerData& data,
                          MPPlayerControllerGOAL* controller,
                          uint32_t player_id) {
  if (!mp_valid_player_id(player_id) || player_id == data.local_player_id) {
    return;
  }
  data.player_states[player_id] = {};
  if (controller) {
    auto& record = controller->records[player_id];
    const auto runtime = record.runtime;
    record = {};
    record.runtime = runtime;
    record.runtime.mission_flags = 0;
    record.identity.player_id = kMPInvalidPlayerId;
    record.identity.character = MPPlayerCharacter::UNKNOWN;
    record.action.riding_along_player_id = kMPInvalidPlayerId;
  }
}

void mp_seed_peer_roster(MultiplayerData& data,
                         ENetPeer* peer,
                         const MPPlayerControllerGOAL* controller) {
  if (data.session_role != 0 || !peer || !controller) {
    return;
  }
  auto* destination = multiplayer_host_peer_find(data, peer);
  if (!destination || !destination->authenticated) {
    return;
  }
  for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    if (player_id == destination->player_id) {
      continue;
    }
    const auto& record = controller->records[player_id];
    if (!valid_join_record(record) || record.identity.player_id != player_id) {
      continue;
    }
    const auto join = make_join_packet(record, ++data.sequence_num);
    MultiplayerManager::send_to_peer(data, peer, static_cast<int>(MultiplayerChannel::CONTROL),
                                     join, ENET_PACKET_FLAG_RELIABLE);
    if (player_id == data.local_player_id) {
      destination->local_identity_sent = true;
    }
    const auto& cached = data.player_states[player_id];
    if (!cached.state_ready) {
      continue;
    }
    const auto state = make_cached_state_packet(cached, ++data.sequence_num);
    MultiplayerManager::send_to_peer(data, peer, static_cast<int>(MultiplayerChannel::STATE), state,
                                     ENET_PACKET_FLAG_UNSEQUENCED);
  }
}

bool mp_handle_player_state_packet(MultiplayerData& data,
                                   const ENetPacket* packet,
                                   uint32_t sender_player_id,
                                   uint32_t current_time) {
  const auto state = PacketView(packet).as_exact<PacketPlayerState>(PacketType::STATE_UPDATE);
  if (!state || !valid_player_state(*state) ||
      !mp_player_id_allowed_from_sender(data, sender_player_id, state->player_id)) {
    return false;
  }
  auto& cached = data.player_states[state->player_id];
  if (!mp_sequence_is_newer(state->header.sequenceNum, cached.last_sequence_num)) {
    return false;
  }
  cached.player_id = state->player_id;
  cached.state_ready = true;
  cached.status = state->status;
  cached.x = state->x;
  cached.y = state->y;
  cached.z = state->z;
  cached.angle = state->angle;
  cached.vel_x = state->vel_x;
  cached.vel_y = state->vel_y;
  cached.vel_z = state->vel_z;
  cached.send_tick = state->send_tick;
  cached.receive_tick = current_time;
  cached.state_id = state->state_id;
  cached.level_hash = state->level_hash;
  cached.darkjak_stage = state->darkjak_stage;
  cached.buttons = state->buttons;
  cached.leftx = state->leftx;
  cached.lefty = state->lefty;
  cached.rightx = state->rightx;
  cached.righty = state->righty;
  cached.respawn_flags = state->respawn_flags;
  cached.spectator_only = state->spectator_only;
  cached.cam_angle_y = state->cam_angle_y;
  cached.riding_veh_id = state->riding_veh_id;
  cached.riding_seat_index = state->riding_seat_index;
  cached.scene_active = state->scene_active;
  cached.equipped_weapon = state->equipped_weapon;
  cached.turret_active = state->turret_active;
  cached.action_seq = state->action_seq;
  cached.action_state_id = state->action_state_id;
  cached.riding_along_player_id = state->riding_along_player_id;
  cached.mission_flags = state->mission_flags;
  cached.last_sequence_num = state->header.sequenceNum;
  memcpy(&cached.veh_state, &state->veh_state, sizeof(cached.veh_state));
  if (data.session_role == 0 && state->status == static_cast<uint8_t>(MultiplayerStatus::IN_GAME)) {
    if (auto* session = multiplayer_host_peer_for_player_id(data, state->player_id)) {
      if (session->bootstrap_sent_once) {
        session->bootstrap_pending = false;
        session->bootstrap_sent_once = false;
      }
    }
  }
  return true;
}

bool mp_handle_turret_state_packet(MultiplayerData& data,
                                   const ENetPacket* packet,
                                   uint32_t sender_player_id) {
  const auto state = PacketView(packet).as_exact<PacketTurretState>(PacketType::TURRET_SYNC);
  if (!state || !mp_float_is_finite(state->roty) || !mp_float_is_finite(state->rotx) ||
      !mp_player_id_allowed_from_sender(data, sender_player_id, state->player_id)) {
    return false;
  }
  auto& cached = data.player_states[state->player_id];
  if (!mp_sequence_is_newer(state->header.sequenceNum, cached.last_turret_sequence_num)) {
    return false;
  }
  cached.last_turret_sequence_num = state->header.sequenceNum;
  if (state->turret_aid != 0) {
    cached.turret_roty = state->roty;
    cached.turret_rotx = state->rotx;
  }
  return true;
}

bool mp_handle_join_packet(MultiplayerData& data,
                           const ENetPacket* packet,
                           uint32_t sender_player_id,
                           MPPlayerControllerGOAL* controller,
                           bool* character_assignment_mismatch) {
  if (character_assignment_mismatch) {
    *character_assignment_mismatch = false;
  }
  const auto join = PacketView(packet).as_exact<PacketJoin>(PacketType::EVENT_JOIN);
  if (join && data.session_role == 0) {
    const auto* session = multiplayer_host_peer_for_player_id(data, sender_player_id);
    if (!session || !mp_valid_player_character(session->character) ||
        join->character != session->character) {
      if (character_assignment_mismatch) {
        *character_assignment_mismatch = true;
      }
      return false;
    }
  }
  if (!join || !controller || !mp_valid_player_id(join->player_id) ||
      join->player_id == controller->local_player_id || join->player_id == data.local_player_id ||
      !mp_player_id_allowed_from_sender(data, sender_player_id, join->player_id) ||
      !mp_valid_player_character(join->character) || !valid_player_name(join->player_name)) {
    return false;
  }
  auto& cached = data.player_states[join->player_id];
  cached.identity_ready = true;
  cached.player_id = join->player_id;
  cached.character = join->character;
  memcpy(cached.player_name, join->player_name, sizeof(cached.player_name));

  auto& record = controller->records[join->player_id];
  const auto runtime = record.runtime;
  record = {};
  record.runtime = runtime;
  record.runtime.mission_flags = 0;
  record.identity.player_id = join->player_id;
  record.identity.character = join->character;
  record.identity.identity_ready = 1;
  record.identity.joined = 1;
  record.connection.connected = 1;
  record.action.riding_along_player_id = kMPInvalidPlayerId;
  memcpy(record.identity.name, join->player_name, sizeof(record.identity.name));
  lg::info("[MP-Join] Registered player {} with character {}.", join->player_id,
           static_cast<uint32_t>(join->character));
  if (data.session_role == 0) {
    if (auto* session = multiplayer_host_peer_for_player_id(data, join->player_id)) {
      session->identity_ready = true;
      if (data.host_game_active) {
        session->bootstrap_pending = true;
        session->bootstrap_sent_once = false;
        session->last_bootstrap_send_time = 0;
        lg::info("[MP-Bootstrap] Armed bootstrap for player {} after identity registration.",
                 join->player_id);
      }
    }
  }
  return true;
}

void mp_handle_world_state_packet(MultiplayerData& data,
                                  const ENetPacket* packet,
                                  MPWorldSyncStateGOAL* world) {
  const auto state = PacketView(packet).as_exact<PacketWorldState>(PacketType::WORLD_STATE);
  if (!state || !world || data.session_role != 1 || !valid_world_state(*state) ||
      state->player_id != data.host_player_id ||
      !mp_sequence_is_newer(state->header.sequenceNum, data.last_world_sequence)) {
    return;
  }
  data.last_world_sequence = state->header.sequenceNum;
  world->sequence = state->header.sequenceNum;
  world->clock = state->clock;
  world->time_of_day_frame = state->time_of_day_frame;
  world->time_of_day_ratio = state->time_of_day_ratio;
  world->weather_cloud = state->weather_cloud;
  world->weather_fog = state->weather_fog;
  world->weather_rain = state->weather_rain;
  memcpy(world->task_mask, state->task_mask, sizeof(world->task_mask));
  memcpy(world->active_task_mask, state->active_task_mask, sizeof(world->active_task_mask));
}

void mp_handle_bootstrap_packet(const ENetPacket* packet,
                                MPWorldSyncStateGOAL* world,
                                MPBootstrapSyncStateGOAL* bootstrap) {
  const auto state = PacketView(packet).as_exact<PacketBootstrap>(PacketType::BOOTSTRAP);
  if (!state || !world || !bootstrap || !valid_bootstrap_packet(*state)) {
    return;
  }
  world->money = state->money;
  world->gems = state->gems;
  world->skill = state->skill;
  world->clock = state->clock;
  world->time_of_day_frame = state->tod_frame;
  world->time_of_day_ratio = state->tod_ratio;
  world->weather_cloud = state->weather_cloud;
  world->weather_fog = state->weather_fog;
  world->weather_rain = state->weather_rain;
  memcpy(world->task_mask, state->task_mask, sizeof(world->task_mask));
  memcpy(world->active_task_mask, state->active_task_mask, sizeof(world->active_task_mask));
  bootstrap->phase = 1;
  bootstrap->sequence = state->header.sequenceNum;
  bootstrap->host_task = state->host_task;
  memcpy(bootstrap->host_continue, state->host_continue, sizeof(bootstrap->host_continue));
  bootstrap->host_spawn_position[0] = state->x;
  bootstrap->host_spawn_position[1] = state->y;
  bootstrap->host_spawn_position[2] = state->z;
  bootstrap->host_spawn_position[3] = 1.0f;
  bootstrap->host_camera_angle_y = state->cam_angle_y;
  bootstrap->synchronized_aid_count = state->sync_aids_count;
  memcpy(bootstrap->synchronized_aids, state->sync_aids, sizeof(bootstrap->synchronized_aids));
}

void mp_send_player_sync(MultiplayerData& data,
                         MPPlayerControllerGOAL* controller,
                         MPWorldSyncStateGOAL* world,
                         MPBootstrapSyncStateGOAL* bootstrap) {
  if (!controller || !world || !bootstrap || !mp_valid_player_id(controller->local_player_id)) {
    return;
  }
  if (data.session_role == 0) {
    data.local_player_id = controller->local_player_id;
    data.host_player_id = controller->host_player_id;
  } else if (controller->local_player_id != data.local_player_id) {
    return;
  }
  const auto* local = joined_record(controller, controller->local_player_id);
  if (!local) {
    return;
  }
  send_join_packet(data, *local);

  PacketPlayerState state = {};
  state.header.type = PacketType::STATE_UPDATE;
  state.header.sequenceNum = ++data.sequence_num;
  state.player_id = data.local_player_id;
  state.status = static_cast<uint8_t>(data.join_status.load());
  state.x = local->transform.position[0];
  state.y = local->transform.position[1];
  state.z = local->transform.position[2];
  state.angle = local->transform.angle;
  state.vel_x = local->transform.velocity[0];
  state.vel_y = local->transform.velocity[1];
  state.vel_z = local->transform.velocity[2];
  state.send_tick = enet_time_get();
  state.state_id = local->action.target_state_id;
  state.level_hash = local->transform.level;
  data.local_traffic_level_hash = state.level_hash;
  state.darkjak_stage = local->action.darkjak_stage;
  state.buttons = local->input.buttons;
  state.leftx = local->input.leftx;
  state.lefty = local->input.lefty;
  state.rightx = local->input.rightx;
  state.righty = local->input.righty;
  state.respawn_flags = local->action.respawn_flags;
  state.spectator_only = local->identity.spectator_only;
  state.cam_angle_y = local->input.camera_angle_y;
  state.riding_veh_id = local->vehicle.vehicle_id;
  state.riding_seat_index = local->vehicle.seat_index;
  state.scene_active = local->action.scene_state;
  state.equipped_weapon = local->input.equipped_weapon;
  state.turret_active = local->vehicle.turret_active;
  state.action_seq = local->action.authoritative_sequence;
  state.action_state_id = local->action.action_state_id;
  state.riding_along_player_id = local->action.riding_along_player_id;
  state.mission_flags = local->runtime.mission_flags;
  memcpy(&state.veh_state, &local->vehicle.state, sizeof(state.veh_state));
  MultiplayerManager::broadcast(data, static_cast<int>(MultiplayerChannel::STATE), state,
                                ENET_PACKET_FLAG_UNSEQUENCED);

  if (!local->identity.spectator_only && local->vehicle.turret_active &&
      local->vehicle.vehicle_id != 0) {
    PacketTurretState turret = {};
    turret.header.type = PacketType::TURRET_SYNC;
    turret.header.sequenceNum = ++data.sequence_num;
    turret.player_id = data.local_player_id;
    turret.turret_aid = local->vehicle.vehicle_id;
    turret.roty = local->vehicle.turret_roty;
    turret.rotx = local->vehicle.turret_rotx;
    MultiplayerManager::broadcast(data, static_cast<int>(MultiplayerChannel::STATE), turret,
                                  ENET_PACKET_FLAG_UNSEQUENCED);
  }
  send_world_state(data, data.local_player_id, *world);

  if (data.session_role != 0 || !data.host || data.host->connectedPeers == 0 ||
      data.join_status != static_cast<int>(MultiplayerStatus::IN_GAME) ||
      !has_host_continue(bootstrap)) {
    return;
  }
  const uint32_t now = enet_time_get();
  for (auto& session : data.host_peer_sessions) {
    if (!session.authenticated || !session.identity_ready || !session.bootstrap_pending ||
        !session.peer ||
        (session.last_bootstrap_send_time != 0 && now - session.last_bootstrap_send_time < 500)) {
      continue;
    }
    session.last_bootstrap_send_time = now;
    const auto packet = make_bootstrap_packet(*local, *world, *bootstrap, ++data.sequence_num);
    if (MultiplayerManager::send_to_peer(data, session.peer,
                                         static_cast<int>(MultiplayerChannel::CONTROL), packet,
                                         ENET_PACKET_FLAG_RELIABLE)) {
      if (!session.bootstrap_sent_once) {
        lg::info("[MP-Bootstrap] Queued authoritative bootstrap for player {}.",
                 session.player_id);
      }
      session.bootstrap_sent_once = true;
    }
  }
}

void mp_receive_player_sync(MultiplayerData& data,
                            MPPlayerControllerGOAL* controller,
                            MPWorldSyncStateGOAL*,
                            MPBootstrapSyncStateGOAL*) {
  if (!controller) {
    return;
  }
  if (data.session_role == 0 && mp_valid_player_id(controller->local_player_id)) {
    data.local_player_id = controller->local_player_id;
  }
  controller->host_player_id = data.host_player_id;
  for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    if (player_id == data.local_player_id) {
      continue;
    }
    const auto& cached = data.player_states[player_id];
    if (!cached.identity_ready) {
      continue;
    }
    auto& record = controller->records[player_id];
    if (!record.identity.joined) {
      record.identity.player_id = player_id;
      record.identity.character = cached.character;
      record.identity.identity_ready = 1;
      record.identity.joined = 1;
      record.connection.connected = 1;
      memcpy(record.identity.name, cached.player_name, sizeof(record.identity.name));
    }
    if (cached.state_ready) {
      CachedPlayerState predicted = cached;
      const uint32_t now = enet_time_get();
      const uint32_t age_ms = cached.receive_tick != 0 && now >= cached.receive_tick
                                  ? std::min<uint32_t>(now - cached.receive_tick, 100)
                                  : 0;
      const float dt = static_cast<float>(age_ms) * 0.001f;
      predicted.x += predicted.vel_x * dt;
      predicted.y += predicted.vel_y * dt;
      predicted.z += predicted.vel_z * dt;
      copy_cached_state_to_record(predicted, record);
    }
  }
}
