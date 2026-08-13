#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "enet/enet.h"
#include "game/multiplayer/multiplayer_api.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_peer_registry.h"
#include "game/multiplayer/multiplayer_port_mapping.h"
#include "game/multiplayer/multiplayer_port_mapping_internal.h"
#include "game/multiplayer/multiplayer_port_mapping_route.h"
#include "game/multiplayer/multiplayer_preferences.h"
#include "game/multiplayer/multiplayer_scanner.h"
#include "game/multiplayer/multiplayer_security.h"
#include "game/multiplayer/multiplayer_session.h"
#include "game/multiplayer/multiplayer_version.h"
#include "game/multiplayer/multiplayer_wire_codec.h"
#include "game/multiplayer/sync/event_sync.h"
#include "game/multiplayer/sync/player_sync.h"
#include "game/multiplayer/sync/traffic_sync.h"
#include "gtest/gtest.h"

#include "third-party/SDL/include/SDL3/SDL.h"

namespace {
struct LocalEnetPair {
  ENetHost* receiver = nullptr;
  ENetHost* sender = nullptr;
  ENetPeer* receiver_peer = nullptr;
  ENetPeer* sender_peer = nullptr;

  ~LocalEnetPair() {
    if (sender) {
      enet_host_destroy(sender);
    }
    if (receiver) {
      enet_host_destroy(receiver);
    }
  }
};

bool connect_local_enet_pair(LocalEnetPair& pair) {
  ENetAddress receiver_address = {};
  receiver_address.host = ENET_HOST_ANY;
  receiver_address.port = 0;
  pair.receiver = enet_host_create(&receiver_address, 1, 2, 0, 0);
  pair.sender = enet_host_create(nullptr, 1, 2, 0, 0);
  if (!pair.receiver || !pair.sender) {
    return false;
  }

  ENetAddress sender_address = {};
  if (enet_address_set_host(&sender_address, "127.0.0.1") != 0) {
    return false;
  }
  sender_address.port = pair.receiver->address.port;
  pair.sender_peer = enet_host_connect(pair.sender, &sender_address, 2, 0);
  if (!pair.sender_peer) {
    return false;
  }

  for (int attempt = 0; attempt < 200; ++attempt) {
    ENetEvent event = {};
    while (enet_host_service(pair.receiver, &event, 0) > 0) {
      if (event.type == ENET_EVENT_TYPE_CONNECT) {
        pair.receiver_peer = event.peer;
      } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        enet_packet_destroy(event.packet);
      }
    }
    while (enet_host_service(pair.sender, &event, 0) > 0) {
      if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        enet_packet_destroy(event.packet);
      }
    }
    enet_host_flush(pair.sender);
    enet_host_flush(pair.receiver);
    if (pair.receiver_peer && pair.sender_peer->state == ENET_PEER_STATE_CONNECTED &&
        pair.receiver_peer->state == ENET_PEER_STATE_CONNECTED) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

void authenticate(MultiplayerSecurity& host, MultiplayerSecurity& client) {
  ASSERT_TRUE(host.set_local_version("v1.0.0"));
  ASSERT_TRUE(client.set_local_version("v1.0.0"));
  MultiplayerDatagram server_hello;
  ASSERT_TRUE(host.make_server_hello(server_hello));
  SecurityReceiveResult client_proof =
      client.receive(1, server_hello.bytes.data(), server_hello.size);
  ASSERT_EQ(client_proof.kind, SecurityReceiveKind::HANDSHAKE);
  ASSERT_GT(client_proof.response.size, 0);

  SecurityReceiveResult server_proof =
      host.receive(0, client_proof.response.bytes.data(), client_proof.response.size);
  ASSERT_EQ(server_proof.kind, SecurityReceiveKind::HANDSHAKE);
  ASSERT_TRUE(host.authenticated());

  SecurityReceiveResult client_result =
      client.receive(1, server_proof.response.bytes.data(), server_proof.response.size);
  ASSERT_EQ(client_result.kind, SecurityReceiveKind::HANDSHAKE);
  ASSERT_TRUE(client.authenticated());
}
}  // namespace

TEST(MultiplayerPacket, SaturatingQuantizationRejectsNonFiniteValues) {
  EXPECT_EQ(mp_pack_float_q(std::numeric_limits<float>::quiet_NaN()), 0);
  EXPECT_EQ(mp_pack_float_scaled(std::numeric_limits<float>::infinity(), 10.0f), 0);
  EXPECT_EQ(mp_pack_float_scaled(100000.0f, 10.0f), (std::numeric_limits<int16_t>::max)());
  EXPECT_EQ(mp_pack_float_scaled(-100000.0f, 10.0f), (std::numeric_limits<int16_t>::min)());
}

TEST(MultiplayerPacket, SequenceComparisonHandlesWraparound) {
  EXPECT_TRUE(mp_sequence_is_newer(1, 0));
  EXPECT_TRUE(mp_sequence_is_newer(0, (std::numeric_limits<uint32_t>::max)()));
  EXPECT_FALSE(mp_sequence_is_newer(9, 10));
  EXPECT_FALSE(mp_sequence_is_newer(10, 10));
}

TEST(MultiplayerPacket, PacketViewRejectsTruncationUnknownTypesAndTrailingData) {
  PacketPlayerState state = {};
  state.header.type = PacketType::STATE_UPDATE;
  EXPECT_EQ(sizeof(PacketPlayerState), 170u);
  std::vector<uint8_t> bytes(sizeof(state) + 1);
  memcpy(bytes.data(), &state, sizeof(state));
  ENetPacket packet = {};
  packet.data = bytes.data();

  packet.dataLength = sizeof(PacketHeader) - 1;
  EXPECT_FALSE(PacketView(&packet).has_header());

  packet.dataLength = sizeof(state) + 1;
  EXPECT_FALSE(PacketView(&packet).as_exact<PacketPlayerState>(PacketType::STATE_UPDATE));

  packet.dataLength = sizeof(state);
  bytes[0] = static_cast<uint8_t>(PacketType::COUNT);
  EXPECT_EQ(PacketView(&packet).type(), PacketType::COUNT);
  EXPECT_FALSE(PacketView(&packet).as_exact<PacketPlayerState>(PacketType::STATE_UPDATE));
}

TEST(MultiplayerPlayerRegistry, GoalLayoutsAndOffsetsMatch) {
  EXPECT_EQ(sizeof(MPPlayerIdentityGOAL), 48u);
  EXPECT_EQ(sizeof(MPPlayerConnectionStateGOAL), 16u);
  EXPECT_EQ(sizeof(MPPlayerTransformStateGOAL), 48u);
  EXPECT_EQ(sizeof(MPPlayerActionStateGOAL), 32u);
  EXPECT_EQ(sizeof(MPPlayerInputStateGOAL), 16u);
  EXPECT_EQ(sizeof(MPPlayerVehicleStateGOAL), 96u);
  EXPECT_EQ(sizeof(MPTargetGhostRecordGOAL), 96u);
  EXPECT_EQ(sizeof(MPPlayerRuntimeStateGOAL), 224u);
  EXPECT_EQ(sizeof(MPPlayerRecordGOAL), 480u);
  EXPECT_EQ(sizeof(MPPlayerCharacterConfigGOAL), sizeof(uint32_t) * kMPMaxPlayers);
  EXPECT_EQ(sizeof(MPPlayerControllerGOAL),
            sizeof(MPPlayerRecordGOAL) * kMPMaxPlayers + sizeof(uint32_t) * 4);
  EXPECT_EQ(sizeof(MPWorldSyncStateGOAL), 176u);
  EXPECT_EQ(sizeof(MPBootstrapSyncStateGOAL), 592u);
  EXPECT_EQ(offsetof(MPPlayerIdentityGOAL, character), 4u);
  EXPECT_EQ(offsetof(MPPlayerIdentityGOAL, identity_ready), 8u);
  EXPECT_EQ(offsetof(MPPlayerIdentityGOAL, spectator_only), 11u);
  EXPECT_EQ(offsetof(MPPlayerIdentityGOAL, name), 12u);
  EXPECT_EQ(offsetof(MPPlayerConnectionStateGOAL, latest_state_sequence), 4u);
  EXPECT_EQ(offsetof(MPPlayerConnectionStateGOAL, connected), 12u);
  EXPECT_EQ(offsetof(MPPlayerTransformStateGOAL, velocity), 16u);
  EXPECT_EQ(offsetof(MPPlayerTransformStateGOAL, angle), 32u);
  EXPECT_EQ(offsetof(MPPlayerTransformStateGOAL, level), 36u);
  EXPECT_EQ(offsetof(MPPlayerActionStateGOAL, scene_state), 16u);
  EXPECT_EQ(offsetof(MPPlayerActionStateGOAL, last_replayed_sequence), 20u);
  EXPECT_EQ(offsetof(MPPlayerActionStateGOAL, riding_along_player_id), 24u);
  EXPECT_EQ(offsetof(MPPlayerInputStateGOAL, camera_angle_y), 8u);
  EXPECT_EQ(offsetof(MPPlayerVehicleStateGOAL, turret_roty), 8u);
  EXPECT_EQ(offsetof(MPPlayerVehicleStateGOAL, state), 16u);
  EXPECT_EQ(offsetof(MPTargetGhostRecordGOAL, state_id), 48u);
  EXPECT_EQ(offsetof(MPTargetGhostRecordGOAL, buttons), 56u);
  EXPECT_EQ(offsetof(MPTargetGhostRecordGOAL, camera_angle_y), 64u);
  EXPECT_EQ(offsetof(MPTargetGhostRecordGOAL, last_update), 72u);
  EXPECT_EQ(offsetof(MPTargetGhostRecordGOAL, active), 80u);
  EXPECT_EQ(offsetof(MPPlayerRuntimeStateGOAL, ghost), 16u);
  EXPECT_EQ(offsetof(MPPlayerRuntimeStateGOAL, presentation_position), 112u);
  EXPECT_EQ(offsetof(MPPlayerRuntimeStateGOAL, last_fresh_input_time), 144u);
  EXPECT_EQ(offsetof(MPPlayerRuntimeStateGOAL, last_state_packet_id), 160u);
  EXPECT_EQ(offsetof(MPPlayerRuntimeStateGOAL, interpolation_angle), 188u);
  EXPECT_EQ(offsetof(MPPlayerRuntimeStateGOAL, pad_index), 204u);
  EXPECT_EQ(offsetof(MPPlayerRuntimeStateGOAL, flags), 208u);
  EXPECT_EQ(offsetof(MPPlayerRecordGOAL, connection), 48u);
  EXPECT_EQ(offsetof(MPPlayerRecordGOAL, transform), 64u);
  EXPECT_EQ(offsetof(MPPlayerRecordGOAL, action), 112u);
  EXPECT_EQ(offsetof(MPPlayerRecordGOAL, input), 144u);
  EXPECT_EQ(offsetof(MPPlayerRecordGOAL, vehicle), 160u);
  EXPECT_EQ(offsetof(MPPlayerRecordGOAL, runtime), 256u);
  const size_t controller_record_bytes = sizeof(MPPlayerRecordGOAL) * kMPMaxPlayers;
  EXPECT_EQ(offsetof(MPPlayerControllerGOAL, local_player_id), controller_record_bytes);
  EXPECT_EQ(offsetof(MPPlayerControllerGOAL, host_player_id),
            controller_record_bytes + sizeof(uint32_t));
  EXPECT_EQ(offsetof(MPPlayerControllerGOAL, reserved),
            controller_record_bytes + sizeof(uint32_t) * 2);
  EXPECT_EQ(offsetof(MPWorldSyncStateGOAL, sequence), 12u);
  EXPECT_EQ(offsetof(MPWorldSyncStateGOAL, clock), 16u);
  EXPECT_EQ(offsetof(MPWorldSyncStateGOAL, task_mask), 48u);
  EXPECT_EQ(offsetof(MPBootstrapSyncStateGOAL, host_continue), 16u);
  EXPECT_EQ(offsetof(MPBootstrapSyncStateGOAL, host_spawn_position), 48u);
  EXPECT_EQ(offsetof(MPBootstrapSyncStateGOAL, synchronized_aids), 80u);
}

TEST(MultiplayerEvents, StampsValidatesDecodesAndDispatchesSourcePlayerId) {
  MultiplayerData outbound;
  outbound.local_player_id = 3;
  MPEventBufferGOAL outgoing = {};
  outgoing.out_count = 1;
  outgoing.out_events[0].etype = 1;
  outgoing.out_events[0].payload_size = 4;
  outgoing.out_events[0].source_player_id = 0;
  outgoing.out_events[0].data[0] = 0x5a;

  mp_send_game_events(outbound, &outgoing);

  EXPECT_EQ(outgoing.out_count, 0u);
  EXPECT_EQ(outgoing.out_events[0].source_player_id, 3u);

  MPEvent source_event = {};
  source_event.etype = 1;
  source_event.payload_size = 4;
  source_event.source_player_id = 2;
  source_event.data[0] = 0xa5;
  std::vector<uint8_t> encoded;
  ASSERT_TRUE(mp_encode_game_event(source_event, 17, encoded));

  PacketGameEvent decoded = {};
  ASSERT_TRUE(mp_decode_game_event(encoded.data(), encoded.size(), decoded));
  EXPECT_EQ(decoded.header.sequenceNum, 17u);
  EXPECT_EQ(decoded.source_player_id, 2u);
  EXPECT_EQ(decoded.event_id, 1u);
  EXPECT_EQ(decoded.payload[0], 0xa5u);

  MultiplayerData inbound;
  inbound.session_role = 0;
  inbound.local_player_id = 3;
  ENetPacket packet = {};
  packet.data = encoded.data();
  packet.dataLength = encoded.size();
  ASSERT_TRUE(mp_handle_game_event_packet(inbound, &packet, 2));

  MPEventBufferGOAL received = {};
  mp_receive_game_events(inbound, &received);
  ASSERT_EQ(received.in_count, 1u);
  EXPECT_EQ(received.in_events[0].source_player_id, 2u);
  EXPECT_EQ(received.in_events[0].etype, 1u);
  EXPECT_EQ(received.in_events[0].data[0], 0xa5u);

  EXPECT_FALSE(mp_handle_game_event_packet(inbound, &packet, 2));
  mp_receive_game_events(inbound, &received);
  EXPECT_EQ(received.in_count, 1u);

  source_event.source_player_id = 1;
  ASSERT_TRUE(mp_encode_game_event(source_event, 1, encoded));
  packet.data = encoded.data();
  packet.dataLength = encoded.size();
  ASSERT_TRUE(mp_handle_game_event_packet(inbound, &packet, 1));
  mp_receive_game_events(inbound, &received);
  EXPECT_EQ(received.in_count, 2u);

  ASSERT_TRUE(mp_encode_game_event(source_event, 18, encoded));
  packet.data = encoded.data();
  packet.dataLength = encoded.size();
  EXPECT_FALSE(mp_handle_game_event_packet(inbound, &packet, 2));
  mp_receive_game_events(inbound, &received);
  EXPECT_EQ(received.in_count, 2u);

  source_event.source_player_id = kMPInvalidPlayerId;
  EXPECT_FALSE(mp_encode_game_event(source_event, 19, encoded));
}

TEST(MultiplayerOwnership, PlayerIdsRemainCanonicalAcrossEnemyPedestrianAndVehicleState) {
  MPEnemyState enemy = {};
  enemy.owner_player_id = 3;
  enemy.focus_player_id = 2;
  EXPECT_EQ(enemy.owner_player_id, 3u);
  EXPECT_EQ(enemy.focus_player_id, 2u);
  EXPECT_EQ(offsetof(MPEnemyState, focus_player_id), 52u);
  EXPECT_EQ(offsetof(MPEnemyState, owner_player_id), 57u);

  MPPedestrianState pedestrian = {};
  pedestrian.target_player_id = 3;
  EXPECT_EQ(pedestrian.target_player_id, 3u);
  EXPECT_EQ(offsetof(MPPedestrianState, target_player_id), 41u);

  MPVehicleState vehicle = {};
  vehicle.target_player_id = 3;
  vehicle.rider_player_ids[0] = 0;
  vehicle.rider_player_ids[1] = 3;
  vehicle.rider_player_ids[2] = kMPVehicleCivilianRiderId;
  vehicle.rider_player_ids[3] = kMPInvalidPlayerId;
  EXPECT_EQ(vehicle.target_player_id, 3u);
  EXPECT_EQ(vehicle.rider_player_ids[0], 0u);
  EXPECT_EQ(vehicle.rider_player_ids[1], 3u);
  EXPECT_EQ(vehicle.rider_player_ids[2], kMPVehicleCivilianRiderId);
  EXPECT_EQ(vehicle.rider_player_ids[3], kMPInvalidPlayerId);
  EXPECT_EQ(offsetof(MPVehicleState, target_player_id), 7u);
  EXPECT_EQ(offsetof(MPVehicleState, rider_player_ids), 64u);
}

TEST(MultiplayerOwnership, BattleAidNamespaceScalesWithPlayerCapacity) {
  EXPECT_EQ(mp_player_id_bit_count(2), 1u);
  EXPECT_EQ(mp_player_id_bit_count(4), 2u);
  EXPECT_EQ(mp_player_id_bit_count(8), 3u);
  EXPECT_EQ(mp_player_id_bit_count(16), 4u);
  EXPECT_EQ(mp_player_id_bit_count(32), 5u);

  std::array<uint32_t, kMPMaxPlayers> aids = {};
  for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    aids[player_id] = 0x60000000u | (player_id << kMPBattleSpawnCounterBits) | 1u;
    EXPECT_GE(aids[player_id], 0x60000000u);
    EXPECT_LT(aids[player_id], 0x70000000u);
  }
  for (uint32_t left = 0; left < kMPMaxPlayers; ++left) {
    for (uint32_t right = left + 1; right < kMPMaxPlayers; ++right) {
      EXPECT_NE(aids[left], aids[right]);
    }
  }
}

TEST(MultiplayerJoin, RegistersPlayerThreeAsDaxterAndAllowsDuplicateCharacters) {
  MultiplayerData data;
  data.local_player_id = 1;
  MPPlayerControllerGOAL controller = {};
  controller.local_player_id = 1;
  controller.host_player_id = 1;
  controller.records[1].identity.joined = 1;
  controller.records[1].identity.player_id = 1;
  controller.records[1].identity.character = MPPlayerCharacter::DAXTER;

  PacketJoin join = {};
  join.header.type = PacketType::EVENT_JOIN;
  join.player_id = 3;
  join.character = MPPlayerCharacter::DAXTER;
  memcpy(join.player_name, "PlayerThree", sizeof("PlayerThree"));
  ENetPacket packet = {};
  packet.data = reinterpret_cast<uint8_t*>(&join);
  packet.dataLength = sizeof(join);

  mp_handle_join_packet(data, &packet, 3, &controller);

  EXPECT_EQ(controller.records[3].identity.player_id, 3u);
  EXPECT_EQ(controller.records[3].identity.character, MPPlayerCharacter::DAXTER);
  EXPECT_EQ(controller.records[3].identity.joined, 1u);
  EXPECT_STREQ(controller.records[3].identity.name, "PlayerThree");
}

TEST(MultiplayerJoin, RejectsMalformedIdentityWithoutMutatingSlot) {
  MultiplayerData data;
  data.local_player_id = 1;
  MPPlayerControllerGOAL controller = {};
  controller.local_player_id = 1;
  memcpy(controller.records[3].identity.name, "before", sizeof("before"));

  PacketJoin join = {};
  join.header.type = PacketType::EVENT_JOIN;
  join.player_id = 3;
  join.character = MPPlayerCharacter::DAXTER;
  memset(join.player_name, 'x', sizeof(join.player_name));
  ENetPacket packet = {};
  packet.data = reinterpret_cast<uint8_t*>(&join);
  packet.dataLength = sizeof(join);

  mp_handle_join_packet(data, &packet, 3, &controller);
  EXPECT_STREQ(controller.records[3].identity.name, "before");
  EXPECT_EQ(controller.records[3].identity.joined, 0u);

  memcpy(join.player_name, "bad-name", sizeof("bad-name"));
  mp_handle_join_packet(data, &packet, 3, &controller);
  EXPECT_STREQ(controller.records[3].identity.name, "before");

  join.player_id = 1;
  memcpy(join.player_name, "ValidName", sizeof("ValidName"));
  mp_handle_join_packet(data, &packet, 1, &controller);
  EXPECT_EQ(controller.records[1].identity.joined, 0u);
}

TEST(MultiplayerJoin, EnforcesTheHostAssignedCharacter) {
  MultiplayerData data;
  data.session_role = 0;
  data.local_player_id = 0;
  data.host_player_id = 0;
  data.host_peer_sessions[0].authenticated = true;
  data.host_peer_sessions[0].player_id = 1;
  data.host_peer_sessions[0].character = MPPlayerCharacter::JAK;
  MPPlayerControllerGOAL controller = {};
  controller.local_player_id = 0;
  controller.host_player_id = 0;

  PacketJoin join = {};
  join.header.type = PacketType::EVENT_JOIN;
  join.player_id = 1;
  join.character = MPPlayerCharacter::DAXTER;
  memcpy(join.player_name, "AssignedOne", sizeof("AssignedOne"));
  ENetPacket packet = {};
  packet.data = reinterpret_cast<uint8_t*>(&join);
  packet.dataLength = sizeof(join);

  bool assignment_mismatch = false;
  EXPECT_FALSE(mp_handle_join_packet(data, &packet, 1, &controller, &assignment_mismatch));
  EXPECT_TRUE(assignment_mismatch);
  EXPECT_EQ(controller.records[1].identity.joined, 0u);

  join.character = MPPlayerCharacter::JAK;
  EXPECT_TRUE(mp_handle_join_packet(data, &packet, 1, &controller, &assignment_mismatch));
  EXPECT_FALSE(assignment_mismatch);
  EXPECT_EQ(controller.records[1].identity.character, MPPlayerCharacter::JAK);
}

TEST(MultiplayerPlayerRegistry, StateBeforeJoinRemainsVacantUntilIdentityArrives) {
  MultiplayerData data;
  data.local_player_id = 2;
  MPPlayerControllerGOAL controller = {};
  controller.local_player_id = 2;
  controller.host_player_id = 2;
  MPWorldSyncStateGOAL world = {};
  MPBootstrapSyncStateGOAL bootstrap = {};

  PacketPlayerState state = {};
  state.header.type = PacketType::STATE_UPDATE;
  state.header.sequenceNum = 7;
  state.player_id = 3;
  state.status = static_cast<uint8_t>(MultiplayerStatus::IN_GAME);
  state.spectator_only = 1;
  state.x = 12.5f;
  state.riding_along_player_id = 1;
  ENetPacket state_packet = {};
  state_packet.data = reinterpret_cast<uint8_t*>(&state);
  state_packet.dataLength = sizeof(state);
  mp_handle_player_state_packet(data, &state_packet, 3, 100);
  mp_receive_player_sync(data, &controller, &world, &bootstrap);

  EXPECT_TRUE(data.player_states[3].state_ready);
  EXPECT_EQ(controller.records[3].identity.joined, 0u);

  PacketJoin join = {};
  join.header.type = PacketType::EVENT_JOIN;
  join.player_id = 3;
  join.character = MPPlayerCharacter::DAXTER;
  memcpy(join.player_name, "DaxterThree", sizeof("DaxterThree"));
  ENetPacket join_packet = {};
  join_packet.data = reinterpret_cast<uint8_t*>(&join);
  join_packet.dataLength = sizeof(join);
  mp_handle_join_packet(data, &join_packet, 3, &controller);
  mp_receive_player_sync(data, &controller, &world, &bootstrap);

  EXPECT_EQ(controller.records[3].identity.joined, 1u);
  EXPECT_EQ(controller.records[3].identity.state_ready, 1u);
  EXPECT_EQ(controller.records[3].identity.spectator_only, 1u);
  EXPECT_FLOAT_EQ(controller.records[3].transform.position[0], 12.5f);
  EXPECT_EQ(controller.records[3].action.riding_along_player_id, 1u);
}

TEST(MultiplayerPlayerRegistry, RejectsSpoofsStaleStateAndLocalSlotClear) {
  MultiplayerData data;
  data.session_role = 0;
  data.local_player_id = 3;
  MPPlayerControllerGOAL controller = {};
  controller.local_player_id = 3;
  controller.records[2].identity.joined = 1;
  controller.records[2].identity.player_id = 2;
  controller.records[3].identity.joined = 1;
  controller.records[3].identity.player_id = 3;

  PacketPlayerState state = {};
  state.header.type = PacketType::STATE_UPDATE;
  state.header.sequenceNum = 10;
  state.player_id = 1;
  state.x = 99.0f;
  ENetPacket packet = {};
  packet.data = reinterpret_cast<uint8_t*>(&state);
  packet.dataLength = sizeof(state);
  mp_handle_player_state_packet(data, &packet, 2, 100);
  EXPECT_FALSE(data.player_states[1].state_ready);

  state.player_id = 2;
  state.x = 10.0f;
  mp_handle_player_state_packet(data, &packet, 2, 101);
  EXPECT_FLOAT_EQ(data.player_states[2].x, 10.0f);
  state.header.sequenceNum = 9;
  state.x = 9.0f;
  mp_handle_player_state_packet(data, &packet, 2, 102);
  EXPECT_FLOAT_EQ(data.player_states[2].x, 10.0f);

  state.header.sequenceNum = 11;
  state.spectator_only = 2;
  EXPECT_FALSE(mp_handle_player_state_packet(data, &packet, 2, 103));
  state.spectator_only = 0;
  state.header.sequenceNum = 12;
  state.riding_along_player_id = 2;
  EXPECT_FALSE(mp_handle_player_state_packet(data, &packet, 2, 104));
  state.riding_along_player_id = kMPMaxPlayers;
  EXPECT_FALSE(mp_handle_player_state_packet(data, &packet, 2, 105));

  mp_clear_player_slot(data, &controller, 3);
  EXPECT_EQ(controller.records[3].identity.joined, 1u);
  mp_clear_player_slot(data, &controller, 2);
  EXPECT_FALSE(data.player_states[2].state_ready);
  EXPECT_EQ(controller.records[2].identity.player_id, kMPInvalidPlayerId);
  EXPECT_EQ(controller.records[2].action.riding_along_player_id, kMPInvalidPlayerId);
}

TEST(MultiplayerBootstrap, AppliesValidPacketToWorldAndBootstrapBuffers) {
  PacketBootstrap bootstrap = {};
  bootstrap.header.type = PacketType::BOOTSTRAP;
  bootstrap.header.sequenceNum = 7;
  bootstrap.money = 12.0f;
  bootstrap.gems = 3.0f;
  bootstrap.skill = 4.0f;
  bootstrap.x = 1.0f;
  bootstrap.y = 2.0f;
  bootstrap.z = 3.0f;
  bootstrap.host_continue[0] = 't';
  bootstrap.host_continue[1] = 'o';
  bootstrap.host_continue[2] = 'm';
  bootstrap.host_continue[3] = 'b';
  bootstrap.sync_aids_count = 1;
  bootstrap.sync_aids[0] = 42;

  ENetPacket packet = {};
  packet.data = reinterpret_cast<uint8_t*>(&bootstrap);
  packet.dataLength = sizeof(bootstrap);
  MPWorldSyncStateGOAL world = {};
  MPBootstrapSyncStateGOAL sync = {};

  mp_handle_bootstrap_packet(&packet, &world, &sync);

  EXPECT_FLOAT_EQ(world.money, 12.0f);
  EXPECT_FLOAT_EQ(world.gems, 3.0f);
  EXPECT_FLOAT_EQ(world.skill, 4.0f);
  EXPECT_FLOAT_EQ(sync.host_spawn_position[0], 1.0f);
  EXPECT_FLOAT_EQ(sync.host_spawn_position[1], 2.0f);
  EXPECT_FLOAT_EQ(sync.host_spawn_position[2], 3.0f);
  EXPECT_EQ(sync.synchronized_aid_count, 1u);
  EXPECT_EQ(sync.synchronized_aids[0], 42u);
  EXPECT_EQ(sync.phase, 1u);
}

TEST(MultiplayerBootstrap, RejectsInvalidPacketWithoutMutatingGoalBuffers) {
  PacketBootstrap bootstrap = {};
  bootstrap.header.type = PacketType::BOOTSTRAP;
  bootstrap.money = std::numeric_limits<float>::quiet_NaN();
  memset(bootstrap.host_continue, 'x', sizeof(bootstrap.host_continue));

  ENetPacket packet = {};
  packet.data = reinterpret_cast<uint8_t*>(&bootstrap);
  packet.dataLength = sizeof(bootstrap);
  MPWorldSyncStateGOAL world = {};
  MPBootstrapSyncStateGOAL sync = {};

  mp_handle_bootstrap_packet(&packet, &world, &sync);

  EXPECT_EQ(sync.phase, 0u);
  EXPECT_FLOAT_EQ(world.money, 0.0f);
  EXPECT_FLOAT_EQ(sync.host_spawn_position[0], 0.0f);
}

TEST(MultiplayerWorldState, AcceptsOnlyAuthenticatedHostAndRejectsStaleSequences) {
  MultiplayerData data;
  data.session_role = 1;
  data.local_player_id = 3;
  data.host_player_id = 2;
  MPWorldSyncStateGOAL world = {};
  PacketWorldState state = {};
  state.header.type = PacketType::WORLD_STATE;
  state.header.sequenceNum = 8;
  state.player_id = 2;
  state.clock = 123;
  state.weather_rain = 0.75f;
  ENetPacket packet = {};
  packet.data = reinterpret_cast<uint8_t*>(&state);
  packet.dataLength = sizeof(state);

  mp_handle_world_state_packet(data, &packet, &world);
  EXPECT_EQ(world.clock, 123u);
  EXPECT_FLOAT_EQ(world.weather_rain, 0.75f);
  EXPECT_EQ(world.sequence, 8u);

  state.header.sequenceNum = 7;
  state.weather_rain = 0.25f;
  mp_handle_world_state_packet(data, &packet, &world);
  EXPECT_FLOAT_EQ(world.weather_rain, 0.75f);

  state.header.sequenceNum = 9;
  state.player_id = 1;
  mp_handle_world_state_packet(data, &packet, &world);
  EXPECT_EQ(world.sequence, 8u);

  data.session_role = 0;
  state.player_id = 2;
  mp_handle_world_state_packet(data, &packet, &world);
  EXPECT_EQ(world.sequence, 8u);
}

TEST(MultiplayerBootstrap, RequestResetsPendingSendState) {
  MultiplayerData data;
  data.session_role = 0;
  data.host_peer_sessions[0].authenticated = true;
  data.host_peer_sessions[0].identity_ready = true;
  data.host_peer_sessions[0].bootstrap_pending = false;
  data.host_peer_sessions[0].bootstrap_sent_once = true;
  data.host_peer_sessions[0].last_bootstrap_send_time = 99;

  multiplayer_request_bootstrap(data);

  EXPECT_TRUE(data.host_peer_sessions[0].bootstrap_pending);
  EXPECT_FALSE(data.host_peer_sessions[0].bootstrap_sent_once);
  EXPECT_EQ(data.host_peer_sessions[0].last_bootstrap_send_time, 0u);
}

TEST(MultiplayerJoin, DisconnectResetsSentState) {
  MultiplayerData data;
  data.local_join_identity_sent = true;

  multiplayer_clear_remote_peer_state(data);

  EXPECT_FALSE(data.local_join_identity_sent);
}

TEST(MultiplayerJoin, RetriesAndResendsOncePerAuthenticatedSession) {
  struct ScopedEnet {
    bool initialized = enet_initialize() == 0;
    ~ScopedEnet() {
      if (initialized) {
        enet_deinitialize();
      }
    }
  } enet;
  ASSERT_TRUE(enet.initialized);

  LocalEnetPair pair;
  ASSERT_TRUE(connect_local_enet_pair(pair));

  MultiplayerSecurity host_security;
  MultiplayerData data;
  data.session_role = 1;
  data.local_player_id = 3;
  data.host_player_id = 0;
  ASSERT_TRUE(host_security.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(data.security.start_client(host_security.invite_for_address("127.0.0.1"), parsed_host,
                                         parsed_port));
  authenticate(host_security, data.security);

  MPPlayerControllerGOAL controller = {};
  controller.local_player_id = 3;
  controller.host_player_id = 0;
  auto& local = controller.records[3];
  local.identity.joined = 1;
  local.identity.identity_ready = 1;
  local.identity.player_id = 3;
  local.identity.character = MPPlayerCharacter::JAK;
  memcpy(local.identity.name, "Ranger", sizeof("Ranger"));
  MPWorldSyncStateGOAL world = {};
  MPBootstrapSyncStateGOAL bootstrap = {};

  mp_send_player_sync(data, &controller, &world, &bootstrap);
  EXPECT_FALSE(data.local_join_identity_sent);
  EXPECT_EQ(data.packet_scheduler.queued_packet_count(), 0u);

  data.host = pair.sender;
  data.server_peer = pair.sender_peer;
  mp_send_player_sync(data, &controller, &world, &bootstrap);
  EXPECT_TRUE(data.local_join_identity_sent);
  EXPECT_EQ(data.packet_scheduler.queued_packet_count(), 2u);

  mp_send_player_sync(data, &controller, &world, &bootstrap);
  EXPECT_EQ(data.packet_scheduler.queued_packet_count(), 2u);

  multiplayer_clear_remote_peer_state(data);
  EXPECT_FALSE(data.local_join_identity_sent);
  mp_send_player_sync(data, &controller, &world, &bootstrap);
  EXPECT_TRUE(data.local_join_identity_sent);
  EXPECT_EQ(data.packet_scheduler.queued_packet_count(), 2u);
}

TEST(MultiplayerJoin, SpectatorOnlyStateSurvivesSendReceiveReplayAndClear) {
  struct ScopedEnet {
    bool initialized = enet_initialize() == 0;
    ~ScopedEnet() {
      if (initialized) {
        enet_deinitialize();
      }
    }
  } enet;
  ASSERT_TRUE(enet.initialized);

  LocalEnetPair pair;
  ASSERT_TRUE(connect_local_enet_pair(pair));
  MultiplayerSecurity host_security;
  ASSERT_TRUE(host_security.start_host(26210));

  MultiplayerData sender;
  sender.host = pair.sender;
  sender.server_peer = pair.sender_peer;
  sender.session_role = 1;
  sender.local_player_id = 3;
  sender.host_player_id = 0;
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(sender.security.start_client(host_security.invite_for_address("127.0.0.1"),
                                           parsed_host, parsed_port));
  authenticate(host_security, sender.security);

  MPPlayerControllerGOAL sender_controller = {};
  sender_controller.local_player_id = 3;
  sender_controller.host_player_id = 0;
  auto& local = sender_controller.records[3];
  local.identity.player_id = 3;
  local.identity.character = MPPlayerCharacter::JAK;
  local.identity.identity_ready = 1;
  local.identity.joined = 1;
  local.identity.spectator_only = 1;
  local.transform.position[0] = 32.0f;
  memcpy(local.identity.name, "Observer", sizeof("Observer"));
  MPWorldSyncStateGOAL sender_world = {};
  MPBootstrapSyncStateGOAL sender_bootstrap = {};
  mp_send_player_sync(sender, &sender_controller, &sender_world, &sender_bootstrap);

  PacketPlayerState sent_state = {};
  bool saw_state = false;
  sender.packet_scheduler.flush_plain(
      sender.stats,
      [&](ENetPeer*, int, const uint8_t* bytes, size_t size, PacketType type, size_t,
          ENetPacketFlag) {
        if (type == PacketType::STATE_UPDATE) {
          EXPECT_EQ(size, sizeof(PacketPlayerState));
          if (size == sizeof(PacketPlayerState)) {
            memcpy(&sent_state, bytes, sizeof(sent_state));
          }
          saw_state = true;
        }
        return true;
      });
  ASSERT_TRUE(saw_state);
  EXPECT_EQ(sent_state.spectator_only, 1u);

  MultiplayerData receiver;
  receiver.local_player_id = 2;
  MPPlayerControllerGOAL receiver_controller = {};
  receiver_controller.local_player_id = 2;
  PacketJoin join = {};
  join.header.type = PacketType::EVENT_JOIN;
  join.player_id = 3;
  join.character = MPPlayerCharacter::JAK;
  memcpy(join.player_name, "Observer", sizeof("Observer"));
  ENetPacket join_packet = {};
  join_packet.data = reinterpret_cast<uint8_t*>(&join);
  join_packet.dataLength = sizeof(join);
  ASSERT_TRUE(mp_handle_join_packet(receiver, &join_packet, 3, &receiver_controller));

  ENetPacket state_packet = {};
  state_packet.data = reinterpret_cast<uint8_t*>(&sent_state);
  state_packet.dataLength = sizeof(sent_state);
  ASSERT_TRUE(mp_handle_player_state_packet(receiver, &state_packet, 3, 100));
  MPWorldSyncStateGOAL receiver_world = {};
  MPBootstrapSyncStateGOAL receiver_bootstrap = {};
  mp_receive_player_sync(receiver, &receiver_controller, &receiver_world, &receiver_bootstrap);
  EXPECT_EQ(receiver_controller.records[3].identity.spectator_only, 1u);

  MPPlayerControllerGOAL replay_controller = {};
  replay_controller.local_player_id = 2;
  mp_receive_player_sync(receiver, &replay_controller, &receiver_world, &receiver_bootstrap);
  EXPECT_EQ(replay_controller.records[3].identity.spectator_only, 1u);

  sent_state.header.sequenceNum += 1;
  sent_state.spectator_only = 0;
  sent_state.x = 64.0f;
  ASSERT_TRUE(mp_handle_player_state_packet(receiver, &state_packet, 3, 101));
  mp_receive_player_sync(receiver, &receiver_controller, &receiver_world, &receiver_bootstrap);
  EXPECT_EQ(receiver_controller.records[3].identity.spectator_only, 0u);
  EXPECT_EQ(receiver_controller.records[3].identity.state_ready, 1u);
  EXPECT_FLOAT_EQ(receiver_controller.records[3].transform.position[0], 64.0f);
}

TEST(MultiplayerBootstrap, PlayerStateAcknowledgesSentBootstrap) {
  MultiplayerData data;
  data.session_role = 0;
  data.host_peer_sessions[0].authenticated = true;
  data.host_peer_sessions[0].player_id = 1;
  data.host_peer_sessions[0].bootstrap_pending = true;
  data.host_peer_sessions[0].bootstrap_sent_once = true;
  PacketPlayerState state = {};
  state.header.type = PacketType::STATE_UPDATE;
  state.header.sequenceNum = 1;
  state.player_id = 1;
  state.status = static_cast<uint8_t>(MultiplayerStatus::IN_GAME);
  ENetPacket packet = {};
  packet.data = reinterpret_cast<uint8_t*>(&state);
  packet.dataLength = sizeof(state);
  data.local_player_id = 0;
  mp_handle_player_state_packet(data, &packet, 1, 100);

  EXPECT_FALSE(data.host_peer_sessions[0].bootstrap_pending);
  EXPECT_FALSE(data.host_peer_sessions[0].bootstrap_sent_once);
}

TEST(MultiplayerBootstrap, PlayerStateDoesNotAcknowledgeUnsentBootstrap) {
  MultiplayerData data;
  data.session_role = 0;
  data.host_peer_sessions[0].authenticated = true;
  data.host_peer_sessions[0].player_id = 1;
  data.host_peer_sessions[0].bootstrap_pending = true;
  data.host_peer_sessions[0].bootstrap_sent_once = false;
  PacketPlayerState state = {};
  state.header.type = PacketType::STATE_UPDATE;
  state.header.sequenceNum = 1;
  state.player_id = 1;
  state.status = static_cast<uint8_t>(MultiplayerStatus::IN_GAME);
  ENetPacket packet = {};
  packet.data = reinterpret_cast<uint8_t*>(&state);
  packet.dataLength = sizeof(state);
  data.local_player_id = 0;
  mp_handle_player_state_packet(data, &packet, 1, 100);

  EXPECT_TRUE(data.host_peer_sessions[0].bootstrap_pending);
  EXPECT_FALSE(data.host_peer_sessions[0].bootstrap_sent_once);
}

TEST(MultiplayerPacket, CountedPayloadRejectsOverflowAndTrailingData) {
  std::array<uint8_t, 32> bytes = {};
  ENetPacket packet = {};
  packet.data = bytes.data();
  packet.dataLength = bytes.size();
  PacketView view(&packet);
  EXPECT_FALSE(view.has_counted_payload((std::numeric_limits<uint32_t>::max)(),
                                        (std::numeric_limits<size_t>::max)(), 17));
  EXPECT_FALSE(view.has_counted_payload(1, 4, 17));
  packet.dataLength = 21;
  EXPECT_TRUE(view.has_counted_payload(1, 4, 17));
}

TEST(MultiplayerPacket, DirectionPolicyMatchesHostAndClientRoles) {
  for (uint8_t value = 0; value < static_cast<uint8_t>(PacketType::COUNT); ++value) {
    const PacketType type = static_cast<PacketType>(value);
    const bool host_only = type == PacketType::BOOTSTRAP || type == PacketType::WORLD_STATE ||
                           type == PacketType::SESSION_WELCOME ||
                           type == PacketType::PALACE_SQUID_SYNC || type == PacketType::WIDOW_SYNC;
    EXPECT_TRUE(mp_packet_direction_allowed(type, 0));
    EXPECT_EQ(mp_packet_direction_allowed(type, 1), !host_only);
  }
  EXPECT_FALSE(mp_packet_direction_allowed(PacketType::STATE_UPDATE, -1));
  EXPECT_FALSE(mp_packet_direction_allowed(PacketType::COUNT, 0));
}

TEST(MultiplayerTraffic, RankedSourceValidationUsesLocalAuthorityMap) {
  MultiplayerData host;
  host.session_role = 0;
  host.local_player_id = 0;
  host.host_player_id = 0;
  mp_set_traffic_authority_map(host, 0x02020000u, 0);  // [0, 0, 2, 2]

  EXPECT_TRUE(mp_validate_traffic_source(host, 0, 0));
  EXPECT_TRUE(mp_validate_traffic_source(host, 2, 2));
  EXPECT_FALSE(mp_validate_traffic_source(host, 1, 1));
  EXPECT_FALSE(mp_validate_traffic_source(host, 2, 1));

  MultiplayerData client;
  client.session_role = 1;
  client.local_player_id = 3;
  client.host_player_id = 0;
  mp_set_traffic_authority_map(client, 0xff02ff00u, 2);  // Spectator selects root 2.

  EXPECT_TRUE(mp_validate_traffic_source(client, 2, 0));
  EXPECT_FALSE(mp_validate_traffic_source(client, 0, 0));
  EXPECT_FALSE(mp_validate_traffic_source(client, 2, 2));
}

TEST(MultiplayerTraffic, TrafficNetIdsEncodeClassOriginAndSequence) {
  for (uint8_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    const uint32_t ped_id = mp_make_traffic_net_id(
        kMPTrafficPedestrianNetIdClass, player_id, 1u);
    const uint32_t vehicle_id = mp_make_traffic_net_id(
        kMPTrafficVehicleNetIdClass, player_id, 32768u);
    const uint32_t player_vehicle_id = mp_make_traffic_net_id(
        kMPPlayerVehicleNetIdClass, player_id, kMPTrafficNetIdSequenceMask);

    EXPECT_EQ(mp_traffic_net_id_class(ped_id), kMPTrafficPedestrianNetIdClass);
    EXPECT_EQ(mp_traffic_net_id_origin(ped_id), player_id);
    EXPECT_EQ(mp_traffic_net_id_sequence(ped_id), 1u);
    EXPECT_NE(ped_id, vehicle_id);
    EXPECT_NE(vehicle_id, player_vehicle_id);
    EXPECT_TRUE(mp_validate_pedestrian_net_id(ped_id, player_id));
    EXPECT_TRUE(mp_validate_vehicle_net_id(vehicle_id));
    EXPECT_TRUE(mp_validate_vehicle_net_id(player_vehicle_id));
  }

  EXPECT_EQ(mp_make_traffic_net_id(kMPTrafficPedestrianNetIdClass, kMPMaxPlayers, 1u), 0u);
  EXPECT_EQ(mp_make_traffic_net_id(0x30000000u, 0, 1u), 0u);
  EXPECT_EQ(mp_make_traffic_net_id(
                kMPTrafficVehicleNetIdClass, 0, kMPTrafficNetIdSequenceMask + 1u),
            0u);
}

TEST(MultiplayerTraffic, TrafficNetIdValidationRejectsSpoofedAndLegacyIds) {
  EXPECT_TRUE(mp_validate_pedestrian_net_id(0x13000001u, 3));
  EXPECT_FALSE(mp_validate_pedestrian_net_id(0x13000001u, 2));
  EXPECT_FALSE(mp_validate_pedestrian_net_id(0x14000001u, 0));
  EXPECT_FALSE(mp_validate_pedestrian_net_id(1u, 0));
  EXPECT_FALSE(mp_validate_pedestrian_net_id(0x10000000u, 0));
  EXPECT_TRUE(mp_validate_pedestrian_net_id(0x1300fffeu, 3));
  EXPECT_FALSE(mp_validate_pedestrian_net_id(0x1300ffffu, 3));
  EXPECT_TRUE(mp_validate_pedestrian_net_id(0x71000001u, 0));
  EXPECT_FALSE(mp_validate_pedestrian_net_id(0x72000001u, 0));

  EXPECT_TRUE(mp_validate_vehicle_net_id(0x20008000u));
  EXPECT_TRUE(mp_validate_vehicle_net_id(0x2300fffeu));
  EXPECT_FALSE(mp_validate_vehicle_net_id(0x2300ffffu));
  EXPECT_TRUE(mp_validate_vehicle_net_id(0x41000000u));
  EXPECT_TRUE(mp_validate_vehicle_net_id(0x70000004u));
  EXPECT_FALSE(mp_validate_vehicle_net_id(0x71000001u));
  EXPECT_FALSE(mp_validate_vehicle_net_id(0x70000000u));
  EXPECT_FALSE(mp_validate_vehicle_net_id(0x24008000u));
  EXPECT_FALSE(mp_validate_vehicle_net_id(32768u));
  EXPECT_FALSE(mp_validate_vehicle_net_id(0x20007fffu));
}

TEST(MultiplayerTraffic, SourceHandoffClearsRemoteTrafficState) {
  MultiplayerData data;
  data.local_player_id = 3;
  mp_set_traffic_authority_map(data, 0x02020000u, 2);  // Player 3 follows player 2.
  data.traffic_buffer.pedestrians[0].net_id = 7;
  data.traffic_buffer.vehicles[0].net_id = 9;
  data.last_pedestrian_sequence_by_source[2] = 10;
  data.last_vehicle_sequence_by_source[2] = 11;

  mp_set_traffic_authority_map(data, 0x00000000u, 0);  // Everyone follows player 0.

  EXPECT_EQ(data.traffic_buffer.pedestrians[0].net_id, 0u);
  EXPECT_EQ(data.traffic_buffer.vehicles[0].net_id, 0u);
  EXPECT_EQ(data.last_pedestrian_sequence_by_source[2], 0u);
  EXPECT_EQ(data.last_vehicle_sequence_by_source[2], 0u);
}

TEST(MultiplayerWireCodec, UsesLittleEndianAndRequiresExactConsumption) {
  std::array<uint8_t, 10> bytes = {};
  multiplayer::wire::store_u16_le(bytes.data(), 0x1234);
  multiplayer::wire::store_u64_le(bytes.data() + 2, UINT64_C(0x0102030405060708));
  EXPECT_EQ(bytes[0], 0x34);
  EXPECT_EQ(bytes[9], 0x01);

  multiplayer::wire::Reader reader(bytes.data(), bytes.size());
  uint16_t decoded_u16 = 0;
  uint64_t decoded_u64 = 0;
  EXPECT_TRUE(reader.read_u16(decoded_u16));
  EXPECT_TRUE(reader.read_u64(decoded_u64));
  EXPECT_TRUE(reader.consumed_all());
  EXPECT_EQ(decoded_u16, 0x1234);
  EXPECT_EQ(decoded_u64, UINT64_C(0x0102030405060708));

  multiplayer::wire::Reader truncated(bytes.data(), bytes.size() - 1);
  EXPECT_TRUE(truncated.read_u16(decoded_u16));
  EXPECT_FALSE(truncated.read_u64(decoded_u64));
  EXPECT_FALSE(truncated.consumed_all());
}

TEST(MultiplayerSecurity, InviteRoundTripAndMutualAuthentication) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  const std::string invite = host.invite_for_address("127.0.0.1");
  EXPECT_EQ(invite.size(), 31);
  EXPECT_TRUE(invite.starts_with("jad2mp://"));
  EXPECT_EQ(host.room_code().size(), kMultiplayerRoomCodeLength);
  EXPECT_TRUE(std::all_of(host.room_code().begin(), host.room_code().end(), [](char character) {
    return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
  }));
  EXPECT_EQ(invite.substr(invite.rfind('/') + 1), host.room_code());
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(invite, parsed_host, parsed_port));
  EXPECT_EQ(parsed_host, "127.0.0.1");
  EXPECT_EQ(parsed_port, 26210);
  authenticate(host, client);
}

TEST(MultiplayerSecurity, PersistentRoomCodeOverridesGeneratedRoomCode) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(31415, "i0o1az"));
  EXPECT_EQ(host.room_code(), "I0O1AZ");
  EXPECT_EQ(host.invite_for_address("127.0.0.1"), "jad2mp://127.0.0.1:31415/I0O1AZ");

  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(host.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
  EXPECT_EQ(client.room_code(), "I0O1AZ");
  authenticate(host, client);
}

TEST(MultiplayerSecurity, RoomCodeUsesFreshSaltAndRejectsWrongCode) {
  MultiplayerSecurity first_host;
  MultiplayerSecurity second_host;
  ASSERT_TRUE(first_host.start_host(26210, "ABC123"));
  ASSERT_TRUE(second_host.start_host(26210, "ABC123"));
  ASSERT_TRUE(first_host.set_local_version("v1.0.0"));
  ASSERT_TRUE(second_host.set_local_version("v1.0.0"));
  MultiplayerDatagram first_hello;
  MultiplayerDatagram second_hello;
  ASSERT_TRUE(first_host.make_server_hello(first_hello));
  ASSERT_TRUE(second_host.make_server_hello(second_hello));
  EXPECT_NE(memcmp(first_hello.bytes.data() + 6, second_hello.bytes.data() + 6, 16), 0);
  EXPECT_EQ(first_hello.bytes[4], 1);

  MultiplayerSecurity wrong_client;
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(
      wrong_client.start_client("jad2mp://127.0.0.1:26210/ABC124", parsed_host, parsed_port));
  ASSERT_TRUE(wrong_client.set_local_version("v1.0.0"));
  const auto wrong_proof = wrong_client.receive(1, first_hello.bytes.data(), first_hello.size);
  ASSERT_EQ(wrong_proof.kind, SecurityReceiveKind::HANDSHAKE);
  EXPECT_EQ(
      first_host.receive(0, wrong_proof.response.bytes.data(), wrong_proof.response.size).kind,
      SecurityReceiveKind::REJECTED);
}

TEST(MultiplayerPreferences, ValidatesPortsRoomCodesAndIndependentFallbacks) {
  EXPECT_TRUE(mp_valid_gameplay_port(1024));
  EXPECT_TRUE(mp_valid_gameplay_port(65535));
  EXPECT_FALSE(mp_valid_gameplay_port(1023));
  EXPECT_FALSE(mp_valid_gameplay_port(kMultiplayerDiscoveryPort));

  std::string normalized;
  EXPECT_TRUE(mp_normalize_room_code("i0o1az", normalized, false));
  EXPECT_EQ(normalized, "I0O1AZ");
  EXPECT_FALSE(mp_normalize_room_code("SHORT", normalized, false));
  EXPECT_TRUE(mp_normalize_player_name("Player123", normalized, false));
  EXPECT_EQ(normalized, "Player123");
  EXPECT_TRUE(mp_normalize_player_name("", normalized));
  EXPECT_TRUE(normalized.empty());
  EXPECT_FALSE(mp_normalize_player_name("Player-Name", normalized));
  EXPECT_FALSE(mp_normalize_player_name("ABCDEFGHIJKLMNOPQRSTUVWX", normalized));

  mp_discard_multiplayer_preference_edits();
  SDL_SetModState(SDL_KMOD_SHIFT);
  EXPECT_EQ(mp_edit_multiplayer_preference(2, 'a'), 1);
  EXPECT_EQ(mp_multiplayer_preference_display(2), "A");
  SDL_SetModState(SDL_KMOD_CAPS);
  EXPECT_EQ(mp_edit_multiplayer_preference(2, 'b'), 1);
  EXPECT_EQ(mp_multiplayer_preference_display(2), "AB");
  SDL_SetModState(static_cast<SDL_Keymod>(SDL_KMOD_SHIFT | SDL_KMOD_CAPS));
  EXPECT_EQ(mp_edit_multiplayer_preference(2, 'c'), 1);
  EXPECT_EQ(mp_multiplayer_preference_display(2), "ABc");
  SDL_SetModState(SDL_KMOD_NONE);
  mp_discard_multiplayer_preference_edits();

  const auto valid = mp_parse_multiplayer_preferences(
      R"({"network_port":31415,"room_code":"i0o1az","player_name":"Player123","automatic_port_mapping":false})");
  EXPECT_EQ(valid.network_port, 31415);
  EXPECT_EQ(valid.room_code, "I0O1AZ");
  EXPECT_EQ(valid.player_name, "Player123");
  EXPECT_FALSE(valid.automatic_port_mapping);

  const auto fallback = mp_parse_multiplayer_preferences(
      R"({"network_port":26211,"room_code":"bad!","player_name":"bad-name","automatic_port_mapping":false})");
  EXPECT_EQ(fallback.network_port, kDefaultMultiplayerPort);
  EXPECT_TRUE(fallback.room_code.empty());
  EXPECT_TRUE(fallback.player_name.empty());
  EXPECT_FALSE(fallback.automatic_port_mapping);
}

TEST(MultiplayerSecurity, AuthenticatedLeaveCanTravelInBothDirections) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(host.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
  authenticate(host, client);

  PacketLeave client_leave = {
      {PacketType::EVENT_LEAVE, 1}, 1, MultiplayerLeaveReason::CLIENT_RECONNECTING};
  MultiplayerDatagram encrypted_client_leave;
  ASSERT_TRUE(client.seal(1, client_leave.header.type, &client_leave, sizeof(client_leave),
                          encrypted_client_leave));
  const auto host_result =
      host.receive(0, encrypted_client_leave.bytes.data(), encrypted_client_leave.size);
  ASSERT_EQ(host_result.kind, SecurityReceiveKind::GAMEPLAY);
  ASSERT_EQ(host_result.plaintext.size, sizeof(client_leave));
  PacketLeave decoded_client_leave = {};
  memcpy(&decoded_client_leave, host_result.plaintext.bytes.data(), sizeof(decoded_client_leave));
  EXPECT_EQ(decoded_client_leave.reason, MultiplayerLeaveReason::CLIENT_RECONNECTING);

  PacketLeave host_leave = {{PacketType::EVENT_LEAVE, 2}, 0, MultiplayerLeaveReason::HOST_CLOSED};
  MultiplayerDatagram encrypted_host_leave;
  ASSERT_TRUE(
      host.seal(0, host_leave.header.type, &host_leave, sizeof(host_leave), encrypted_host_leave));
  const auto client_result =
      client.receive(1, encrypted_host_leave.bytes.data(), encrypted_host_leave.size);
  ASSERT_EQ(client_result.kind, SecurityReceiveKind::GAMEPLAY);
  ASSERT_EQ(client_result.plaintext.size, sizeof(host_leave));
  PacketLeave decoded_host_leave = {};
  memcpy(&decoded_host_leave, client_result.plaintext.bytes.data(), sizeof(decoded_host_leave));
  EXPECT_EQ(decoded_host_leave.reason, MultiplayerLeaveReason::HOST_CLOSED);
}

TEST(MultiplayerDisconnect, SendsOneDirectLeaveDespiteQueuedGameplay) {
  struct ScopedEnet {
    bool initialized = enet_initialize() == 0;
    ~ScopedEnet() {
      if (initialized) {
        enet_deinitialize();
      }
    }
  } enet;
  ASSERT_TRUE(enet.initialized);

  LocalEnetPair pair;
  ASSERT_TRUE(connect_local_enet_pair(pair));

  MultiplayerSecurity host_security;
  ASSERT_TRUE(host_security.start_host(pair.receiver->address.port));
  ASSERT_TRUE(host_security.set_local_version("v1.0.0"));

  MultiplayerData client_data;
  client_data.host = pair.sender;
  client_data.server_peer = pair.sender_peer;
  client_data.session_role = 1;
  client_data.initialized = true;
  client_data.reconnect_invite = "jad2mp://127.0.0.1:26210/ABC123";
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client_data.security.start_client(host_security.invite_for_address("127.0.0.1"),
                                                parsed_host, parsed_port));
  authenticate(host_security, client_data.security);

  PacketHeader queued_gameplay = {PacketType::EVENT_GAME, 99};
  ASSERT_TRUE(client_data.packet_scheduler.enqueue_plain(
      pair.sender_peer, static_cast<int>(MultiplayerChannel::CONTROL), &queued_gameplay,
      sizeof(queued_gameplay), PacketType::EVENT_GAME, sizeof(queued_gameplay),
      ENET_PACKET_FLAG_RELIABLE));

  std::atomic<bool> stop_receiver = false;
  std::atomic<int> leave_count = 0;
  std::atomic<int> received_reason = static_cast<int>(MultiplayerLeaveReason::HOST_CLOSED);
  std::thread receiver_thread([&]() {
    while (!stop_receiver.load()) {
      ENetEvent event = {};
      while (enet_host_service(pair.receiver, &event, 1) > 0) {
        if (event.type != ENET_EVENT_TYPE_RECEIVE || !event.packet) {
          continue;
        }
        const auto result = host_security.receive(0, event.packet->data, event.packet->dataLength);
        if (result.kind == SecurityReceiveKind::GAMEPLAY) {
          ENetPacket plaintext = {};
          plaintext.data = const_cast<uint8_t*>(result.plaintext.bytes.data());
          plaintext.dataLength = result.plaintext.size;
          const auto leave = PacketView(&plaintext).as_exact<PacketLeave>(PacketType::EVENT_LEAVE);
          if (leave) {
            received_reason.store(static_cast<int>(leave->reason));
            leave_count.fetch_add(1);
          }
        }
        enet_packet_destroy(event.packet);
      }
    }
  });

  MultiplayerManager::disconnect(client_data);
  pair.sender = nullptr;
  stop_receiver.store(true);
  receiver_thread.join();

  EXPECT_EQ(leave_count.load(), 1);
  EXPECT_EQ(static_cast<MultiplayerLeaveReason>(received_reason.load()),
            MultiplayerLeaveReason::CLIENT_CLOSED);
  EXPECT_FALSE(client_data.initialized);
  EXPECT_TRUE(client_data.reconnect_invite.empty());

  MultiplayerManager::disconnect(client_data);
  EXPECT_EQ(leave_count.load(), 1);
}

TEST(MultiplayerDiscovery, RequiresPortRoomCodeAndValidCapacity) {
  const std::string valid = std::string(DISCOVERY_MAGIC) + "|26210|I0O1AZ|3|4";
  MPDiscoveryResponse response;
  ASSERT_TRUE(mp_parse_discovery_response(valid.data(), valid.size(), response));
  EXPECT_EQ(response.port, 26210);
  EXPECT_EQ(response.room_code, "I0O1AZ");
  EXPECT_EQ(response.current_players, 3u);
  EXPECT_EQ(response.player_limit, 4u);
  EXPECT_FALSE(mp_parse_discovery_response(valid.data(), valid.size() - 1, response));
  EXPECT_FALSE(mp_parse_discovery_response((valid + "x").data(), valid.size() + 1, response));
  const std::string invalid_capacity = std::string(DISCOVERY_MAGIC) + "|26210|I0O1AZ|4|3";
  EXPECT_FALSE(
      mp_parse_discovery_response(invalid_capacity.data(), invalid_capacity.size(), response));
  const std::string old_mode_layout = std::string(DISCOVERY_MAGIC) + "|31415|C|I0O1AZ";
  EXPECT_FALSE(
      mp_parse_discovery_response(old_mode_layout.data(), old_mode_layout.size(), response));
}

TEST(MultiplayerDiscovery, CancellationClearsPrivateResultState) {
  MultiplayerData data;
  data.found_ip = "jad2mp://25.1.2.3:26210/ABC123";
  data.directed_discovery = true;
  data.directed_discovery_address = 1;
  data.directed_discovery_game_port = 26210;
  MultiplayerScanner::stop_search(data);
  EXPECT_TRUE(data.found_ip.empty());
  EXPECT_FALSE(data.directed_discovery);
  EXPECT_EQ(data.directed_discovery_address, 0u);
  EXPECT_EQ(data.directed_discovery_game_port, 0);
}

TEST(MultiplayerDiscovery, DirectedSearchAcceptsOnlyBareIpv4AndValidPort) {
  MultiplayerData data;
  EXPECT_FALSE(MultiplayerScanner::start_direct_search(data, "localhost", 26210));
  EXPECT_FALSE(MultiplayerScanner::start_direct_search(data, "127.0.0.1:26210", 26210));
  EXPECT_FALSE(MultiplayerScanner::start_direct_search(data, "127.0.0.1", 0));
  ASSERT_TRUE(MultiplayerScanner::start_direct_search(data, "127.0.0.1", 26210));
  MultiplayerScanner::stop_search(data);
}

TEST(MultiplayerPortMapping, ClassifiesPublicInviteAddresses) {
  EXPECT_TRUE(mp_is_public_ipv4("8.8.8.8"));
  EXPECT_TRUE(mp_is_public_ipv4("25.1.2.3"));
  EXPECT_TRUE(mp_is_public_ipv4("203.0.114.1"));
  EXPECT_FALSE(mp_is_public_ipv4("127.0.0.1"));
  EXPECT_FALSE(mp_is_public_ipv4("10.0.0.1"));
  EXPECT_FALSE(mp_is_public_ipv4("100.64.0.1"));
  EXPECT_FALSE(mp_is_public_ipv4("169.254.1.1"));
  EXPECT_FALSE(mp_is_public_ipv4("172.16.0.1"));
  EXPECT_FALSE(mp_is_public_ipv4("192.168.0.1"));
  EXPECT_FALSE(mp_is_public_ipv4("198.51.100.1"));
  EXPECT_FALSE(mp_is_public_ipv4("203.0.113.1"));
  EXPECT_FALSE(mp_is_public_ipv4("224.0.0.1"));
  EXPECT_FALSE(mp_is_public_ipv4("999.0.0.1"));
}

TEST(MultiplayerPortMapping, FormatsRouteDiagnosticsWithOptionalMetrics) {
  NetworkAdapterInfo route;
  route.name = "Ethernet";
  route.local_ip = "192.168.50.87";
  route.gateway_ip = "192.168.50.1";
  route.interface_index = 19;
  route.interface_metric = 25;
  EXPECT_EQ(mp_format_port_mapping_route(route),
            "adapter \"Ethernet\" (interface 19), local 192.168.50.87, gateway 192.168.50.1, "
            "metrics route/interface n/a/25");

  auto changed_route = route;
  EXPECT_TRUE(mp_same_port_mapping_route(route, changed_route));
  changed_route.gateway_ip = "26.0.0.1";
  EXPECT_FALSE(mp_same_port_mapping_route(route, changed_route));
}

TEST(MultiplayerPortMapping, TranslatesProtocolErrors) {
  EXPECT_NE(mp_describe_upnp_result(718).find("ConflictInMappingEntry"), std::string::npos);
  EXPECT_NE(mp_describe_natpmp_result(-51).find("not authorized"), std::string::npos);
  EXPECT_NE(mp_describe_natpmp_result(-7).find("does not support"), std::string::npos);
}

TEST(MultiplayerPortMapping, ProjectsHostCopyModeLifecycle) {
  MultiplayerData data;
  EXPECT_EQ(multiplayer_host_copy_mode(data), MultiplayerHostCopyMode::UNAVAILABLE);
  ASSERT_TRUE(data.security.start_host(26210));
  data.initialized = true;
  data.session_role = 0;
  EXPECT_EQ(multiplayer_host_copy_mode(data), MultiplayerHostCopyMode::ROOM_CODE);

  data.internet_host = true;
  data.port_mapping_state = MPPortMappingState::PENDING;
  EXPECT_EQ(multiplayer_host_copy_mode(data), MultiplayerHostCopyMode::ROOM_CODE);
  data.port_mapping_state = MPPortMappingState::FAILED;
  EXPECT_EQ(multiplayer_host_copy_mode(data), MultiplayerHostCopyMode::ROOM_CODE);
  data.port_mapping_state = MPPortMappingState::READY;
  EXPECT_EQ(multiplayer_host_copy_mode(data), MultiplayerHostCopyMode::ROOM_CODE);
  data.port_mapping_external_ip = "8.8.8.8";
  EXPECT_EQ(multiplayer_host_copy_mode(data), MultiplayerHostCopyMode::INVITE);
  data.host_setup_status = static_cast<int>(MultiplayerHostSetupStatus::MAPPING_DISABLED);
  data.port_mapping_state = MPPortMappingState::IDLE;
  data.port_mapping_external_ip.clear();
  EXPECT_EQ(multiplayer_host_copy_mode(data), MultiplayerHostCopyMode::ROOM_CODE);
}

TEST(MultiplayerDirectConnect, ValidatesOptionalRoomCodeAndClearsDraft) {
  auto& data = multiplayer_data();
  pc_multi_reset_direct_connect();
  EXPECT_STREQ(data.direct_port.data(), "26210");
  for (const char character : std::string("25.1.2.3")) {
    EXPECT_EQ(pc_multi_edit_direct_field(0, static_cast<u32>(character)), 1);
  }
  EXPECT_EQ(pc_multi_direct_connect_ready(), 1);
  EXPECT_EQ(pc_multi_edit_direct_field(2, static_cast<u32>('/')), 0);
  const std::string room_code = "I0O1AZ";
  EXPECT_EQ(pc_multi_edit_direct_field(2, static_cast<u32>(room_code[0])), 1);
  EXPECT_EQ(pc_multi_direct_connect_ready(), 0);
  for (const char character : room_code.substr(1)) {
    EXPECT_EQ(pc_multi_edit_direct_field(2, static_cast<u32>(character)), 1);
  }
  EXPECT_EQ(pc_multi_direct_connect_ready(), 1);
  EXPECT_STREQ(data.direct_room_code.data(), "I0O1AZ");
  pc_multi_clear_direct_connect();
  EXPECT_EQ(data.direct_address[0], '\0');
  EXPECT_EQ(data.direct_port[0], '\0');
  EXPECT_EQ(data.direct_room_code[0], '\0');
}

TEST(MultiplayerReconnect, StartsFromSavedInviteAndClearsOnDisconnect) {
  auto& data = multiplayer_data();
  pc_multi_disconnect();

  MultiplayerSecurity host;
  ASSERT_TRUE(host.start_host(26210));
  data.local_version = "v1.0.0";
  data.reconnect_invite = host.invite_for_address("127.0.0.1");

  EXPECT_EQ(pc_multi_reconnect(), 1);
  EXPECT_TRUE(data.initialized);
  EXPECT_EQ(data.session_role, 1);
  EXPECT_FALSE(data.reconnect_invite.empty());

  pc_multi_disconnect();
  EXPECT_TRUE(data.reconnect_invite.empty());
  EXPECT_FALSE(data.reconnect_attempt_active);
}

TEST(MultiplayerPacket, LeavePacketUsesOneByteReasonAndExactSchema) {
  PacketLeave leave = {
      {PacketType::EVENT_LEAVE, 17}, 3, MultiplayerLeaveReason::CLIENT_RECONNECTING};
  ENetPacket packet = {};
  packet.data = reinterpret_cast<uint8_t*>(&leave);
  packet.dataLength = sizeof(leave);

  const auto decoded = PacketView(&packet).as_exact<PacketLeave>(PacketType::EVENT_LEAVE);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->reason, MultiplayerLeaveReason::CLIENT_RECONNECTING);
  EXPECT_EQ(sizeof(leave), kPacketHeaderWireSize + sizeof(uint32_t) + 1);

  packet.dataLength -= 1;
  EXPECT_FALSE(PacketView(&packet).as_exact<PacketLeave>(PacketType::EVENT_LEAVE));
}

TEST(MultiplayerReconnect, UsesAutomaticBackoffUntilCancelled) {
  MultiplayerData data;
  data.session_role = 1;
  data.join_status = (int)MultiplayerStatus::IN_GAME;

  multiplayer_enter_client_reconnect(data, 1000);
  EXPECT_EQ(data.join_status, (int)MultiplayerStatus::RECONNECTING);
  EXPECT_EQ(data.reconnect_next_attempt_time, 1250u);
  EXPECT_TRUE(multiplayer_client_reconnect_due(data, 1250));

  multiplayer_note_client_reconnect_attempt_started(data);
  EXPECT_FALSE(multiplayer_client_reconnect_due(data, 1250));
  multiplayer_note_client_reconnect_failed(data, 2000);
  EXPECT_EQ(data.reconnect_attempt_count, 1u);
  EXPECT_EQ(data.reconnect_next_attempt_time, 2500u);

  multiplayer_note_client_reconnect_attempt_started(data);
  multiplayer_note_client_reconnect_failed(data, 3000);
  EXPECT_EQ(data.reconnect_next_attempt_time, 4000u);
  multiplayer_note_client_reconnect_attempt_started(data);
  multiplayer_note_client_reconnect_failed(data, 5000);
  EXPECT_EQ(data.reconnect_next_attempt_time, 7000u);
  multiplayer_note_client_reconnect_attempt_started(data);
  multiplayer_note_client_reconnect_failed(data, 8000);
  EXPECT_EQ(data.reconnect_attempt_count, 4u);
  EXPECT_EQ(data.reconnect_next_attempt_time, 13000u);

  multiplayer_note_client_reconnect_attempt_started(data);
  multiplayer_note_client_reconnect_failed(data, 14000);
  EXPECT_EQ(data.reconnect_attempt_count, 4u);
  EXPECT_EQ(data.reconnect_next_attempt_time, 19000u);

  multiplayer_cancel_client_reconnect(data);
  EXPECT_FALSE(data.reconnect_attempt_active);
  EXPECT_EQ(data.reconnect_attempt_count, 0u);
  EXPECT_EQ(data.reconnect_next_attempt_time, 0u);
}

TEST(MultiplayerReconnect, HandshakeTimeoutPreservesInviteAndSchedulesRetry) {
  MultiplayerData data;
  data.session_role = 1;
  data.join_status = (int)MultiplayerStatus::RECONNECTING;
  data.reconnect_invite = "jad2mp://127.0.0.1:26210/ABC123";
  data.reconnect_attempt_active = true;

  multiplayer_handle_client_handshake_timeout(data, 5000);

  EXPECT_EQ(data.join_status, (int)MultiplayerStatus::RECONNECTING);
  EXPECT_FALSE(data.reconnect_attempt_active);
  EXPECT_EQ(data.reconnect_next_attempt_time, 5500u);
  EXPECT_EQ(data.reconnect_invite, "jad2mp://127.0.0.1:26210/ABC123");
}

TEST(MultiplayerReconnect, DoesNotCompleteUntilBootstrapRestoresInGameStatus) {
  MultiplayerData data;
  data.session_role = 1;
  data.join_status = (int)MultiplayerStatus::CONNECTING;
  data.reconnect_attempt_active = true;
  data.reconnect_attempt_count = 2;

  multiplayer_note_client_reconnect_authenticated(data);

  EXPECT_TRUE(data.reconnect_waiting_for_bootstrap);
  EXPECT_EQ(data.reconnect_attempt_count, 2u);
  multiplayer_set_status(data, (int)MultiplayerStatus::CONNECTED_LOBBY);
  EXPECT_TRUE(data.reconnect_waiting_for_bootstrap);
  multiplayer_set_status(data, (int)MultiplayerStatus::IN_GAME);
  EXPECT_FALSE(data.reconnect_waiting_for_bootstrap);
  EXPECT_EQ(data.reconnect_attempt_count, 0u);
}

TEST(MultiplayerPeerRegistry, AssignsLowestFreeIdsForArbitraryHostIdAndReusesSlots) {
  MultiplayerData data;
  data.session_role = 0;
  data.host_player_id = kMPMaxPlayers - 1;
  data.local_player_id = kMPMaxPlayers - 1;
  data.session_player_limit = 4;
  std::array<ENetPeer, 3> peers = {};
  std::array<HostPeerSession*, 3> sessions = {};
  std::array<MultiplayerSecurity, 3> clients;
  std::string parsed_host;
  uint16_t parsed_port = 0;

  for (size_t index = 0; index < peers.size(); ++index) {
    sessions[index] = multiplayer_host_peer_allocate(data, &peers[index], 100);
    ASSERT_NE(sessions[index], nullptr);
  }
  EXPECT_EQ(multiplayer_host_pending_peer_count(data), 3u);

  for (size_t index = 0; index < peers.size(); ++index) {
    auto* session = sessions[index];
    ASSERT_TRUE(session->security.start_host(26210, "ABC123"));
    ASSERT_TRUE(session->security.set_local_version("v1.0.0"));
    ASSERT_TRUE(clients[index].start_client(session->security.invite_for_address("127.0.0.1"),
                                            parsed_host, parsed_port));
    ASSERT_TRUE(clients[index].set_local_version("v1.0.0"));
    authenticate(session->security, clients[index]);
    ASSERT_TRUE(multiplayer_host_peer_authenticate(data, *session, 101));
    EXPECT_EQ(session->player_id, index);
  }
  EXPECT_EQ(multiplayer_host_authenticated_peer_count(data), 3u);
  EXPECT_FALSE(multiplayer_host_has_open_player_slot(data));

  multiplayer_host_peer_release(data, &peers[1]);
  ENetPeer replacement = {};
  MultiplayerSecurity replacement_client;
  auto* replacement_session = multiplayer_host_peer_allocate(data, &replacement, 200);
  ASSERT_NE(replacement_session, nullptr);
  ASSERT_TRUE(replacement_session->security.start_host(26210, "ABC123"));
  ASSERT_TRUE(replacement_session->security.set_local_version("v1.0.0"));
  ASSERT_TRUE(replacement_client.start_client(
      replacement_session->security.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
  ASSERT_TRUE(replacement_client.set_local_version("v1.0.0"));
  authenticate(replacement_session->security, replacement_client);
  ASSERT_TRUE(multiplayer_host_peer_authenticate(data, *replacement_session, 201));
  EXPECT_EQ(replacement_session->player_id, 1u);
}

TEST(MultiplayerPeerRegistry, EnforcesRuntimeLimitsAndRejectsInvalidConfiguration) {
  EXPECT_FALSE(multiplayer_valid_player_limit(0));
  EXPECT_FALSE(multiplayer_valid_player_limit(1));
  EXPECT_TRUE(multiplayer_valid_player_limit(2));
  EXPECT_TRUE(multiplayer_valid_player_limit(3));
  EXPECT_TRUE(multiplayer_valid_player_limit(4));
  EXPECT_TRUE(multiplayer_valid_player_limit(kMPMaxPlayers));
  EXPECT_FALSE(multiplayer_valid_player_limit(kMPMaxPlayers + 1));

  MultiplayerData data;
  data.session_role = 0;
  data.local_player_id = 0;
  data.host_player_id = 0;
  data.session_player_limit = 2;
  data.authenticated_peer_count = 1;
  EXPECT_FALSE(multiplayer_host_has_open_player_slot(data));
  data.session_player_limit = 3;
  EXPECT_TRUE(multiplayer_host_has_open_player_slot(data));

  const auto alternating = mp_default_player_character_config();
  EXPECT_TRUE(multiplayer_valid_player_character_config(alternating));
  auto invalid_characters = alternating;
  invalid_characters[2] = MPPlayerCharacter::UNKNOWN;
  EXPECT_FALSE(multiplayer_valid_player_character_config(invalid_characters));
}

TEST(MultiplayerPeerRegistry, AssignsCharactersByCanonicalPlayerId) {
  MultiplayerData data;
  data.session_role = 0;
  data.local_player_id = 0;
  data.host_player_id = 0;
  data.session_player_limit = 4;
  data.session_player_characters = mp_default_player_character_config();
  data.session_player_characters[0] = MPPlayerCharacter::DAXTER;
  data.session_player_characters[1] = MPPlayerCharacter::JAK;
  data.session_player_characters[2] = MPPlayerCharacter::JAK;
  data.session_player_characters[3] = MPPlayerCharacter::DAXTER;
  std::array<ENetPeer, 3> peers = {};
  std::array<MultiplayerSecurity, 3> clients;
  std::string parsed_host;
  uint16_t parsed_port = 0;

  for (uint32_t index = 0; index < peers.size(); ++index) {
    auto* session = multiplayer_host_peer_allocate(data, &peers[index], 100 + index);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->security.start_host(26210, "ABC123"));
    ASSERT_TRUE(session->security.set_local_version("v1.0.0"));
    ASSERT_TRUE(clients[index].start_client(session->security.invite_for_address("127.0.0.1"),
                                            parsed_host, parsed_port));
    ASSERT_TRUE(clients[index].set_local_version("v1.0.0"));
    authenticate(session->security, clients[index]);
    ASSERT_TRUE(multiplayer_host_peer_authenticate(data, *session, 200 + index));
    EXPECT_EQ(session->player_id, index + 1);
    EXPECT_EQ(session->character, data.session_player_characters[index + 1]);
  }
}

TEST(MultiplayerPeerRegistry, KeepsPeerEncryptionSessionsIsolated) {
  MultiplayerSecurity first_host;
  MultiplayerSecurity second_host;
  MultiplayerSecurity first_client;
  MultiplayerSecurity second_client;
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(first_host.start_host(26210, "ABC123"));
  ASSERT_TRUE(second_host.start_host(26210, "ABC123"));
  ASSERT_TRUE(first_client.start_client(first_host.invite_for_address("127.0.0.1"), parsed_host,
                                        parsed_port));
  ASSERT_TRUE(second_client.start_client(second_host.invite_for_address("127.0.0.1"), parsed_host,
                                         parsed_port));
  authenticate(first_host, first_client);
  authenticate(second_host, second_client);

  const PacketHeader header = {PacketType::STATE_UPDATE, 7};
  MultiplayerDatagram encrypted;
  ASSERT_TRUE(first_client.seal(1, header.type, &header, sizeof(header), encrypted));
  EXPECT_EQ(first_host.receive(0, encrypted.bytes.data(), encrypted.size).kind,
            SecurityReceiveKind::GAMEPLAY);
  EXPECT_EQ(second_host.receive(0, encrypted.bytes.data(), encrypted.size).kind,
            SecurityReceiveKind::REJECTED);
}

TEST(MultiplayerSession, HostLeaveIsTerminalAndCancelsReconnect) {
  MultiplayerData data;
  MultiplayerSecurity host;
  ASSERT_TRUE(host.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(
      data.security.start_client(host.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
  ASSERT_TRUE(data.security.set_local_version("v1.0.0"));
  ASSERT_TRUE(host.set_local_version("v1.0.0"));
  authenticate(host, data.security);

  ENetPeer peer = {};
  peer.state = ENET_PEER_STATE_DISCONNECTED;
  data.session_role = 1;
  data.server_peer = &peer;
  data.join_status = (int)MultiplayerStatus::RECONNECTING;
  data.reconnect_attempt_active = true;
  data.player_states[2].last_sequence_num = 99;
  data.inbound_events.push_overwrite({});

  ASSERT_TRUE(multiplayer_handle_host_leave(data, &peer, MultiplayerLeaveReason::HOST_CLOSED));
  EXPECT_EQ(data.join_status, (int)MultiplayerStatus::HOST_LEFT);
  EXPECT_EQ(data.server_peer, nullptr);
  EXPECT_FALSE(data.security.authenticated());
  EXPECT_FALSE(data.reconnect_attempt_active);
  EXPECT_EQ(data.player_states[2].last_sequence_num, 0u);
  EXPECT_TRUE(data.inbound_events.empty());
  EXPECT_FALSE(multiplayer_handle_host_leave(data, &peer, MultiplayerLeaveReason::CLIENT_CLOSED));
}

TEST(MultiplayerReconnect, MissingInviteFailsWithoutStartingClient) {
  auto& data = multiplayer_data();
  pc_multi_disconnect();

  EXPECT_EQ(pc_multi_reconnect(), 0);
  EXPECT_FALSE(data.initialized);
  EXPECT_EQ(data.join_status.load(), static_cast<int>(MultiplayerStatus::FAILED));
}

TEST(MultiplayerSecurity, RejectsMalformedAndNonIpv4Invites) {
  MultiplayerSecurity client;
  std::string host;
  uint16_t port = 0;
  EXPECT_FALSE(client.start_client("127.0.0.1:26210", host, port));
  EXPECT_FALSE(client.start_client("127.0.0.1:26210/ABC123", host, port));
  EXPECT_FALSE(client.start_client("ogmp://127.0.0.1:26210/ABC123", host, port));
  EXPECT_FALSE(client.start_client("jad2mp://example.com:26210/ABC123", host, port));
  EXPECT_FALSE(client.start_client("jad2mp://999.0.0.1:26210/ABC123", host, port));
  EXPECT_FALSE(client.start_client("jad2mp://127.0.0.1:0/ABC123", host, port));
  EXPECT_FALSE(client.start_client("jad2mp://127.0.0.1:26210/AAAAAAAAAAAAAAAAAAAAAA", host, port));
  EXPECT_FALSE(client.start_client("jad2mp://127.0.0.1:26210/ABC123/trailing", host, port));
}

TEST(MultiplayerSecurity, EncryptsAuthenticatesAndRejectsReplay) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(host.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
  authenticate(host, client);

  PacketHeader header = {};
  header.type = PacketType::EVENT_GAME;
  header.sequenceNum = 42;
  MultiplayerDatagram encrypted;
  ASSERT_TRUE(host.seal(0, header.type, &header, sizeof(header), encrypted));

  SecurityReceiveResult first = client.receive(1, encrypted.bytes.data(), encrypted.size);
  ASSERT_EQ(first.kind, SecurityReceiveKind::GAMEPLAY);
  ASSERT_EQ(first.plaintext.size, sizeof(header));

  SecurityReceiveResult replay = client.receive(1, encrypted.bytes.data(), encrypted.size);
  EXPECT_EQ(replay.kind, SecurityReceiveKind::REJECTED);
}

TEST(MultiplayerSecurity, RotatesHostPeerSessionAndPreservesInvite) {
  MultiplayerSecurity host;
  MultiplayerSecurity first_client;
  ASSERT_TRUE(host.start_host(26210));
  const std::string invite = host.invite_for_address("127.0.0.1");
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(first_client.start_client(invite, parsed_host, parsed_port));
  authenticate(host, first_client);

  PacketHeader old_header = {PacketType::EVENT_GAME, 1};
  MultiplayerDatagram old_ciphertext;
  ASSERT_TRUE(
      first_client.seal(1, old_header.type, &old_header, sizeof(old_header), old_ciphertext));

  ASSERT_TRUE(host.rotate_host_peer_session());
  EXPECT_FALSE(host.authenticated());
  EXPECT_TRUE(host.remote_version().empty());
  EXPECT_EQ(host.invite_for_address("127.0.0.1"), invite);
  EXPECT_EQ(host.receive(0, old_ciphertext.bytes.data(), old_ciphertext.size).kind,
            SecurityReceiveKind::REJECTED);

  MultiplayerSecurity replacement_client;
  ASSERT_TRUE(replacement_client.start_client(invite, parsed_host, parsed_port));
  authenticate(host, replacement_client);

  PacketHeader new_header = {PacketType::EVENT_GAME, 2};
  MultiplayerDatagram new_ciphertext;
  ASSERT_TRUE(
      replacement_client.seal(1, new_header.type, &new_header, sizeof(new_header), new_ciphertext));
  EXPECT_EQ(host.receive(0, new_ciphertext.bytes.data(), new_ciphertext.size).kind,
            SecurityReceiveKind::GAMEPLAY);
}

TEST(MultiplayerBootstrap, TracksDeliveryIndependentlyForEachAuthenticatedPeer) {
  MultiplayerData data;
  data.session_role = 0;
  for (uint32_t index = 0; index < 3; ++index) {
    auto& session = data.host_peer_sessions[index];
    session.authenticated = true;
    session.identity_ready = true;
    session.player_id = index + 1;
    session.bootstrap_sent_once = true;
    session.last_bootstrap_send_time = 99 + index;
  }

  multiplayer_request_bootstrap(data);

  for (uint32_t index = 0; index < 3; ++index) {
    const auto& session = data.host_peer_sessions[index];
    EXPECT_TRUE(session.bootstrap_pending);
    EXPECT_FALSE(session.bootstrap_sent_once);
    EXPECT_EQ(session.last_bootstrap_send_time, 0u);
  }
  data.host_peer_sessions[1].bootstrap_pending = false;
  EXPECT_TRUE(data.host_peer_sessions[0].bootstrap_pending);
  EXPECT_FALSE(data.host_peer_sessions[1].bootstrap_pending);
  EXPECT_TRUE(data.host_peer_sessions[2].bootstrap_pending);
}

TEST(MultiplayerPeerRegistry, ReleaseDoesNotClearUnaffectedSessions) {
  MultiplayerData data;
  data.session_role = 0;
  std::array<ENetPeer, 2> peers = {};
  for (uint32_t index = 0; index < 2; ++index) {
    auto& session = data.host_peer_sessions[index];
    session.peer = &peers[index];
    session.authenticated = true;
    session.player_id = index + 1;
  }
  data.authenticated_peer_count = 2;

  multiplayer_host_peer_release(data, &peers[0]);

  EXPECT_EQ(multiplayer_host_authenticated_peer_count(data), 1u);
  EXPECT_EQ(multiplayer_host_peer_find(data, &peers[0]), nullptr);
  const auto* remaining = multiplayer_host_peer_find(data, &peers[1]);
  ASSERT_NE(remaining, nullptr);
  EXPECT_TRUE(remaining->authenticated);
  EXPECT_EQ(remaining->player_id, 2u);
}

TEST(MultiplayerSecurity, RejectsTamperingAndWrongRoomCode) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string invite = host.invite_for_address("127.0.0.1");
  const size_t room_code_offset = invite.rfind('/') + 1;
  invite[room_code_offset] = invite[room_code_offset] == 'A' ? 'B' : 'A';
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(invite, parsed_host, parsed_port));
  ASSERT_TRUE(host.set_local_version("v1.0.0"));
  ASSERT_TRUE(client.set_local_version("v1.0.0"));

  MultiplayerDatagram server_hello;
  ASSERT_TRUE(host.make_server_hello(server_hello));
  SecurityReceiveResult client_proof =
      client.receive(1, server_hello.bytes.data(), server_hello.size);
  ASSERT_GT(client_proof.response.size, 0);
  SecurityReceiveResult rejected =
      host.receive(0, client_proof.response.bytes.data(), client_proof.response.size);
  EXPECT_EQ(rejected.kind, SecurityReceiveKind::REJECTED);
  EXPECT_FALSE(host.authenticated());
}

TEST(MultiplayerVersion, AcceptsCanonicalReleaseAndDevelopmentIdentities) {
  std::string canonical;
  EXPECT_TRUE(mp_canonicalize_semver("v0.3.0-beta-rc-1.0", canonical));
  EXPECT_EQ(canonical, "v0.3.0-beta-rc-1.0");
  EXPECT_TRUE(mp_canonicalize_semver("1.0.1-test-3.1", canonical));
  EXPECT_EQ(canonical, "v1.0.1-test-3.1");
  EXPECT_TRUE(mp_canonicalize_semver("v4.1.4-guns-on-board.0+windows", canonical));
  EXPECT_EQ(canonical, "v4.1.4-guns-on-board.0+windows");

  EXPECT_TRUE(
      mp_resolve_compatibility_identity(kMultiplayerVersionPlaceholder, "366c9e277", canonical));
  EXPECT_EQ(canonical, "dev-366c9e277");
}

TEST(MultiplayerVersion, RejectsMalformedOrUnsafeIdentities) {
  std::string canonical;
  EXPECT_FALSE(mp_canonicalize_semver("v1.0", canonical));
  EXPECT_FALSE(mp_canonicalize_semver("v01.0.0", canonical));
  EXPECT_FALSE(mp_canonicalize_semver("v1.0.0-feature/foo", canonical));
  EXPECT_FALSE(mp_canonicalize_semver("v1.0.0-feature..one", canonical));
  EXPECT_FALSE(mp_canonicalize_semver("v1.0.0-01", canonical));
  EXPECT_FALSE(mp_canonicalize_semver("v1.0.0 trailing", canonical));
  EXPECT_FALSE(mp_canonicalize_semver(std::string(65, '1'), canonical));
  EXPECT_FALSE(
      mp_resolve_compatibility_identity(kMultiplayerVersionPlaceholder, "unknown", canonical));
}

TEST(MultiplayerSecurity, AuthenticatesVersionMismatchBeforeRejectingClient) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(host.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
  ASSERT_TRUE(host.set_local_version("v0.3.0-beta-rc-1.0"));
  ASSERT_TRUE(client.set_local_version("v0.3.0-beta-rc-1.1"));

  MultiplayerDatagram server_hello;
  ASSERT_TRUE(host.make_server_hello(server_hello));
  SecurityReceiveResult client_proof =
      client.receive(1, server_hello.bytes.data(), server_hello.size);
  ASSERT_EQ(client_proof.kind, SecurityReceiveKind::HANDSHAKE);

  SecurityReceiveResult mismatch =
      host.receive(0, client_proof.response.bytes.data(), client_proof.response.size);
  ASSERT_EQ(mismatch.kind, SecurityReceiveKind::VERSION_MISMATCH);
  EXPECT_FALSE(host.authenticated());

  SecurityReceiveResult client_result =
      client.receive(1, mismatch.response.bytes.data(), mismatch.response.size);
  EXPECT_EQ(client_result.kind, SecurityReceiveKind::VERSION_MISMATCH);
  EXPECT_EQ(client.remote_version(), "v0.3.0-beta-rc-1.0");
  EXPECT_FALSE(client.authenticated());

  mismatch.response.bytes[mismatch.response.size - 1] ^= 0x80;
  EXPECT_EQ(client.receive(1, mismatch.response.bytes.data(), mismatch.response.size).kind,
            SecurityReceiveKind::REJECTED);
}

TEST(MultiplayerSecurity, RejectsTruncatedVersionBeforeCopying) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(host.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
  ASSERT_TRUE(host.set_local_version("v1.0.0"));
  ASSERT_TRUE(client.set_local_version("v1.0.0"));

  MultiplayerDatagram hello;
  ASSERT_TRUE(host.make_server_hello(hello));
  constexpr size_t version_length_offset = 6 + 16 + 16 + 32;
  hello.bytes[version_length_offset] = 64;
  EXPECT_EQ(client.receive(1, hello.bytes.data(), hello.size).kind, SecurityReceiveKind::REJECTED);
}

TEST(MultiplayerSecurity, RejectsAlteredCiphertextAndAcceptsReordering) {
  MultiplayerSecurity host;
  MultiplayerSecurity client;
  ASSERT_TRUE(host.start_host(26210));
  std::string parsed_host;
  uint16_t parsed_port = 0;
  ASSERT_TRUE(client.start_client(host.invite_for_address("127.0.0.1"), parsed_host, parsed_port));
  authenticate(host, client);

  PacketHeader header = {PacketType::EVENT_GAME, 1};
  MultiplayerDatagram first;
  MultiplayerDatagram second;
  MultiplayerDatagram third;
  ASSERT_TRUE(host.seal(0, header.type, &header, sizeof(header), first));
  header.sequenceNum = 2;
  ASSERT_TRUE(host.seal(0, header.type, &header, sizeof(header), second));
  header.sequenceNum = 3;
  ASSERT_TRUE(host.seal(0, header.type, &header, sizeof(header), third));

  first.bytes[first.size - 1] ^= 0x80;
  EXPECT_EQ(client.receive(1, first.bytes.data(), first.size).kind, SecurityReceiveKind::REJECTED);
  EXPECT_EQ(client.receive(1, third.bytes.data(), third.size).kind, SecurityReceiveKind::GAMEPLAY);
  EXPECT_EQ(client.receive(1, second.bytes.data(), second.size).kind,
            SecurityReceiveKind::GAMEPLAY);
}
