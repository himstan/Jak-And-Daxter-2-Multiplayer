#include "multiplayer_api.h"

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "common/cross_sockets/XSocket.h"
#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/versions/versions.h"

#include "enet/enet.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/multiplayer/multiplayer.h"
#include "game/multiplayer/multiplayer_direct_connect.h"
#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_packet.h"
#include "game/multiplayer/multiplayer_peer_registry.h"
#include "game/multiplayer/multiplayer_port_mapping.h"
#include "game/multiplayer/multiplayer_preferences.h"
#include "game/multiplayer/multiplayer_protocol.h"
#include "game/multiplayer/multiplayer_scanner.h"
#include "game/multiplayer/multiplayer_session.h"
#include "game/multiplayer/multiplayer_types.h"
#include "game/multiplayer/multiplayer_version.h"
#include "game/multiplayer/pedestrian/multiplayer_pedestrian.h"
#include "game/multiplayer/sync/airlock_sync.h"
#include "game/multiplayer/sync/enemy_sync.h"
#include "game/multiplayer/sync/event_sync.h"
#include "game/multiplayer/sync/palace_squid_sync.h"
#include "game/multiplayer/sync/player_sync.h"
#include "game/multiplayer/sync/traffic_sync.h"
#include "game/multiplayer/sync/widow_sync.h"
#include "game/multiplayer/vehicle/multiplayer_vehicle.h"

#include "third-party/SDL/include/SDL3/SDL.h"

namespace {
constexpr u32 kMinGoalPointer = 0x1000;

bool authentication_address_banned(MultiplayerData& data, uint32_t address, uint32_t now);
void record_authentication_failure(MultiplayerData& data, uint32_t address, uint32_t now);
void expire_pending_handshakes(MultiplayerData& data, uint32_t now);

std::string enet_endpoint_string(const ENetAddress& address) {
  char host[64] = {};
  if (enet_address_get_host_ip(&address, host, sizeof(host)) != 0) {
    return "<unknown>:" + std::to_string(address.port);
  }
  return std::string(host) + ":" + std::to_string(address.port);
}

std::string enet_peer_endpoint_string(const ENetPeer* peer) {
  return peer ? enet_endpoint_string(peer->address) : "<none>";
}

size_t pending_handshake_count(const MultiplayerData& data) {
  return multiplayer_host_pending_peer_count(data);
}

uint16_t enet_local_port(const ENetHost* host) {
  if (!host) {
    return 0;
  }
  if (host->address.port != 0) {
    return host->address.port;
  }
  ENetAddress local = {};
  if (enet_socket_get_address(host->socket, &local) != 0) {
    return 0;
  }
  return local.port;
}

const char* security_receive_kind_name(SecurityReceiveKind kind) {
  switch (kind) {
    case SecurityReceiveKind::HANDSHAKE:
      return "handshake";
    case SecurityReceiveKind::VERSION_MISMATCH:
      return "version-mismatch";
    case SecurityReceiveKind::GAMEPLAY:
      return "gameplay";
    case SecurityReceiveKind::REJECTED:
    default:
      return "rejected";
  }
}

template <typename T>
T* goal_ptr(u32 ptr) {
  if (ptr < kMinGoalPointer || ptr > static_cast<u32>(EE_MAIN_MEM_SIZE - sizeof(T))) {
    return nullptr;
  }
  return Ptr<T>(ptr).c();
}

const uint8_t* goal_byte_array_ptr(u32 ptr, size_t count) {
  if (ptr < kMinGoalPointer || count > EE_MAIN_MEM_SIZE ||
      ptr > static_cast<u32>(EE_MAIN_MEM_SIZE - count)) {
    return nullptr;
  }
  return Ptr<uint8_t>(ptr).c();
}

uint8_t* goal_byte_array_ptr_mut(u32 ptr, size_t count) {
  if (ptr < kMinGoalPointer || count > EE_MAIN_MEM_SIZE ||
      ptr > static_cast<u32>(EE_MAIN_MEM_SIZE - count)) {
    return nullptr;
  }
  return Ptr<uint8_t>(ptr).c();
}

const char* goal_string_data(u32 ptr) {
  if (ptr < kMinGoalPointer || ptr > static_cast<u32>(EE_MAIN_MEM_SIZE - sizeof(String) - 1)) {
    return nullptr;
  }
  auto* str = Ptr<String>(ptr).c();
  if (!str) {
    return nullptr;
  }
  constexpr u32 kMaxBridgeStringLength = 4096;
  if (str->len > kMaxBridgeStringLength ||
      str->len > static_cast<u32>(EE_MAIN_MEM_SIZE) - ptr - sizeof(String) - 1) {
    return nullptr;
  }
  const char* value = str->data();
  return value[str->len] == '\0' ? value : nullptr;
}

bool packet_is_host_relayed(PacketType type) {
  switch (type) {
    case PacketType::STATE_UPDATE:
    case PacketType::EVENT_GAME:
    case PacketType::ENEMY_SYNC:
    case PacketType::TURRET_SYNC:
    case PacketType::AIRLOCK_SYNC:
    case PacketType::PEDESTRIAN_SYNC:
    case PacketType::VEHICLE_SYNC:
    case PacketType::EVENT_JOIN:
    case PacketType::EVENT_LEAVE:
    case PacketType::LOBBY_ACTION:
      return true;
    default:
      return false;
  }
}

void relay_client_packet(MultiplayerData& data,
                         ENetPeer* sender,
                         uint32_t sender_player_id,
                         int channel,
                         const ENetPacket* packet) {
  if (!packet || packet->dataLength < kPacketHeaderWireSize) {
    return;
  }
  std::vector<uint8_t> relayed(packet->data, packet->data + packet->dataLength);
  const uint32_t relay_sequence = ++data.sequence_num;
  memcpy(relayed.data() + sizeof(uint8_t), &relay_sequence, sizeof(relay_sequence));
  MultiplayerManager::broadcast_except(data, sender, channel, relayed.data(), relayed.size(),
                                       (packet->flags & ENET_PACKET_FLAG_RELIABLE)
                                           ? ENET_PACKET_FLAG_RELIABLE
                                           : ENET_PACKET_FLAG_UNSEQUENCED,
                                       sender_player_id);
}

bool handle_session_welcome(MultiplayerData& data, const ENetPacket* packet) {
  const auto welcome =
      PacketView(packet).as_exact<PacketSessionWelcome>(PacketType::SESSION_WELCOME);
  if (data.session_role != 1 || !welcome || !mp_valid_player_id(welcome->player_id) ||
      !mp_valid_player_id(welcome->host_player_id) ||
      welcome->player_id == welcome->host_player_id ||
      !multiplayer_valid_player_limit(welcome->player_limit) ||
      !mp_valid_player_character(welcome->character)) {
    return false;
  }
  data.local_player_id = welcome->player_id;
  data.host_player_id = welcome->host_player_id;
  data.session_player_limit = welcome->player_limit;
  data.local_player_character = welcome->character;
  data.local_join_identity_sent = false;
  data.client_handshake_started_time = 0;
  data.server_last_receive_time = enet_time_get();
  data.connection_phase = static_cast<int>(MultiplayerConnectionPhase::CONNECTED);
  data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::NONE);
  multiplayer_note_client_reconnect_authenticated(data);
  data.join_status = static_cast<int>(MultiplayerStatus::CONNECTED_LOBBY);
  lg::info("[Multiplayer] Session assignment received: local={}, host={}, limit={}, character={}.",
           data.local_player_id, data.host_player_id, data.session_player_limit,
           static_cast<uint32_t>(data.local_player_character));
  return true;
}

bool handle_receive_packet(MultiplayerData& data,
                           ENetPeer* sender,
                           uint32_t sender_player_id,
                           const ENetPacket* packet,
                           MPPlayerControllerGOAL* controller,
                           MPWorldSyncStateGOAL* world,
                           MPBootstrapSyncStateGOAL* bootstrap,
                           uint32_t current_time,
                           int channel) {
  PacketView view(packet);
  if (!view.has_header()) {
    return false;
  }

  const PacketType packet_type = view.type();
  const auto* descriptor =
      multiplayer::schema::packet_descriptor(static_cast<uint8_t>(packet_type));
  if (!descriptor || view.size() - kPacketHeaderWireSize > descriptor->max_payload) {
    return false;
  }
  const int sender_role = data.session_role == 0 ? 1 : 0;
  if (!mp_packet_direction_allowed(packet_type, sender_role)) {
    return false;
  }

  bool accepted = false;
  bool disconnect_sender_after_relay = false;
  bool reject_character_mismatch = false;
  switch (packet_type) {
    case PacketType::STATE_UPDATE:
      accepted = mp_handle_player_state_packet(data, packet, sender_player_id, current_time);
      break;
    case PacketType::EVENT_GAME:
      accepted = mp_handle_game_event_packet(data, packet, sender_player_id);
      break;
    case PacketType::ENEMY_SYNC:
      accepted = mp_handle_enemy_sync_packet(data, packet, sender_player_id, current_time);
      break;
    case PacketType::PEDESTRIAN_SYNC: {
      ENetEvent traffic_event = {};
      traffic_event.type = ENET_EVENT_TYPE_RECEIVE;
      traffic_event.packet = const_cast<ENetPacket*>(packet);
      accepted = handle_pedestrian_sync_packet(traffic_event, data, sender_player_id);
      break;
    }
    case PacketType::VEHICLE_SYNC: {
      ENetEvent traffic_event = {};
      traffic_event.type = ENET_EVENT_TYPE_RECEIVE;
      traffic_event.packet = const_cast<ENetPacket*>(packet);
      accepted = handle_vehicle_sync_packet(traffic_event, data, sender_player_id);
      break;
    }
    case PacketType::TURRET_SYNC:
      accepted = mp_handle_turret_state_packet(data, packet, sender_player_id);
      break;
    case PacketType::PALACE_SQUID_SYNC:
      mp_handle_palace_squid_sync_packet(data, packet, current_time);
      accepted = true;
      break;
    case PacketType::WIDOW_SYNC:
      mp_handle_widow_sync_packet(data, packet, current_time);
      accepted = true;
      break;
    case PacketType::AIRLOCK_SYNC:
      accepted = mp_handle_airlock_sync_packet(data, packet, sender_player_id, current_time);
      break;
    case PacketType::BOOTSTRAP: {
      static uint32_t last_bootstrap_log_time = 0;
      if (last_bootstrap_log_time == 0 || current_time - last_bootstrap_log_time >= 1000) {
        lg::info("[MP-Reconnect] Received BOOTSTRAP from {} (status={}, reconnect_waiting={}).",
                 enet_peer_endpoint_string(sender), data.join_status.load(),
                 data.reconnect_waiting_for_bootstrap);
        last_bootstrap_log_time = current_time;
      }
      mp_handle_bootstrap_packet(packet, world, bootstrap);
      accepted = true;
      break;
    }
    case PacketType::EVENT_JOIN: {
      accepted = mp_handle_join_packet(data, packet, sender_player_id, controller,
                                       &reject_character_mismatch);
      if (accepted && data.session_role == 0) {
        mp_seed_peer_roster(data, sender, controller);
      }
      break;
    }
    case PacketType::EVENT_LEAVE: {
      const auto leave = view.as_exact<PacketLeave>(PacketType::EVENT_LEAVE);
      if (!leave) {
        lg::warn("[MP-Leave] Ignoring malformed EVENT_LEAVE from {} ({} bytes).",
                 enet_peer_endpoint_string(sender), view.size());
        break;
      }
      if (!mp_player_id_allowed_from_sender(data, sender_player_id, leave->player_id)) {
        lg::warn("[MP-Leave] Ignoring spoofed player id {} from {}.", leave->player_id,
                 enet_peer_endpoint_string(sender));
        break;
      }
      if (data.session_role == 1 && leave->player_id == data.host_player_id) {
        accepted = multiplayer_handle_host_leave(data, sender, leave->reason);
      } else {
        accepted = true;
      }
      if (accepted) {
        mp_clear_player_slot(data, controller, leave->player_id);
        if (data.session_role == 0) {
          if (auto* session = multiplayer_host_peer_find(data, sender)) {
            session->identity_ready = false;
            disconnect_sender_after_relay = true;
          }
        }
      }
      lg::info(
          "[MP-Leave] Received EVENT_LEAVE from {} (role={}, reason={}, authenticated={}, "
          "expected_peer={}, accepted={}).",
          enet_peer_endpoint_string(sender), data.session_role, static_cast<int>(leave->reason),
          true, true, accepted);
      break;
    }
    case PacketType::WORLD_STATE:
      mp_handle_world_state_packet(data, packet, world);
      accepted = true;
      break;
    case PacketType::TRAFFIC_AUTHORITY:
      accepted = mp_handle_traffic_authority_packet(data, packet, sender_player_id);
      break;
    case PacketType::SESSION_WELCOME:
      accepted = handle_session_welcome(data, packet);
      break;
    case PacketType::LOBBY_ACTION: {
      const auto lobby = view.as_exact<PacketLobbyAction>(PacketType::LOBBY_ACTION);
      if (!lobby || !controller) {
        break;
      }
      if (lobby->action_type == static_cast<uint8_t>(MPLobbyActionType::SET_CHARACTER)) {
        const auto char_val = static_cast<MPPlayerCharacter>(lobby->value);
        if (mp_valid_player_character(char_val) && mp_valid_player_id(lobby->player_id) &&
            mp_player_id_allowed_from_sender(data, sender_player_id, lobby->player_id)) {
          if (data.session_role == 0) {
            if (auto* session = multiplayer_host_peer_for_player_id(data, lobby->player_id)) {
              session->character = char_val;
            }
            if (lobby->player_id < data.session_player_characters.size()) {
              data.session_player_characters[lobby->player_id] = char_val;
            }
          }
          if (lobby->player_id == data.local_player_id) {
            data.local_player_character = char_val;
          }
          controller->records[lobby->player_id].identity.character = char_val;
          data.player_states[lobby->player_id].character = char_val;
          accepted = true;
          lg::info("[MP-Lobby] Player {} switched character to {}.", lobby->player_id,
                   static_cast<uint32_t>(char_val));
        }
      } else if (lobby->action_type == static_cast<uint8_t>(MPLobbyActionType::SET_APPEARANCE)) {
        if (mp_apply_player_appearance_action(data, sender_player_id, lobby->player_id,
                                              lobby->appearance, controller)) {
          accepted = true;
          lg::info("[MP-Lobby] Player {} changed its complete appearance.", lobby->player_id);
        }
      } else if (lobby->action_type == static_cast<uint8_t>(MPLobbyActionType::START_GAME)) {
        if (data.session_role == 1) {
          data.join_status = static_cast<int>(MultiplayerStatus::GAME_STARTING);
          accepted = true;
          lg::info("[MP-Lobby] Host started game. Transitioning client to GAME_STARTING.");
        }
      }
      break;
    }
    default:
      if (current_time - data.last_traffic_short_packet_debug_time > 2000) {
        lg::warn("[Multiplayer] Ignoring unknown packet type {} ({} bytes).", (int)view.type(),
                 view.size());
        data.last_traffic_short_packet_debug_time = current_time;
      }
      break;
  }
  if (accepted && data.session_role == 0 && packet_is_host_relayed(packet_type)) {
    relay_client_packet(data, sender, sender_player_id, channel, packet);
  }
  if (disconnect_sender_after_relay) {
    enet_peer_disconnect_later(sender, kDisconnectReasonClientClosed);
  }
  if (reject_character_mismatch) {
    lg::warn("[MP-Join] Disconnecting player {} for advertising a character other than the "
             "host-assigned character.",
             sender_player_id);
    enet_peer_disconnect_later(sender, kDisconnectReasonAuthenticationRejected);
  }
  return accepted;
}

void poll_network(MultiplayerData& data,
                  MPPlayerControllerGOAL* controller,
                  MPWorldSyncStateGOAL* world,
                  MPBootstrapSyncStateGOAL* bootstrap) {
  data.stats.calculate_rates(data.host);

  ENetEvent event = {};
  uint32_t current_time = enet_time_get();
  static uint32_t last_security_rejected_log_time = 0;

  while (enet_host_service(data.host, &event, 0) > 0) {
    if (multiplayer_debug_receive_stopped()) {
      if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        enet_packet_destroy(event.packet);
      }
      continue;
    }

    switch (event.type) {
      case ENET_EVENT_TYPE_RECEIVE: {
        auto* security = multiplayer_security_for_peer(data, event.peer);
        if (!security) {
          enet_packet_destroy(event.packet);
          break;
        }
        SecurityReceiveResult secured =
            security->receive(data.session_role, event.packet->data, event.packet->dataLength);
        if (secured.kind != SecurityReceiveKind::GAMEPLAY) {
          if (secured.kind != SecurityReceiveKind::REJECTED ||
              current_time - last_security_rejected_log_time >= 2000) {
            lg::info(
                "[MP-Handshake] Received {} packet from {} (bytes={}, response={}, "
                "authenticated={}, status={}, reconnect_active={}).",
                security_receive_kind_name(secured.kind), enet_peer_endpoint_string(event.peer),
                event.packet->dataLength, secured.response.size, security->authenticated(),
                data.join_status.load(), data.reconnect_attempt_active);
            if (secured.kind == SecurityReceiveKind::REJECTED) {
              last_security_rejected_log_time = current_time;
            }
          }
        }
        bool response_sent = true;
        if (secured.response.size != 0) {
          response_sent = mp_send_raw_packet_to_peer(
              event.peer, static_cast<int>(MultiplayerChannel::CONTROL),
              secured.response.bytes.data(), secured.response.size, ENET_PACKET_FLAG_RELIABLE);
          if (!response_sent) {
            lg::warn(
                "[MP-Handshake] Failed sending {} response to {} ({} bytes, peer_state={}, "
                "reliable_in_transit={}).",
                security_receive_kind_name(secured.kind), enet_peer_endpoint_string(event.peer),
                secured.response.size, static_cast<int>(event.peer->state),
                event.peer->reliableDataInTransit);
          }
        }
        if (!response_sent) {
          lg::warn(
              "[MP-Handshake] Disconnecting peer {} because the secure response could not be sent.",
              enet_peer_endpoint_string(event.peer));
          enet_peer_disconnect_now(event.peer, 0);
        } else if (secured.kind == SecurityReceiveKind::HANDSHAKE && data.session_role == 1 &&
                   !security->authenticated()) {
          lg::info(
              "[MP-Handshake] Client received the server challenge from {}; secure proof response "
              "accepted by ENet ({} bytes).",
              enet_peer_endpoint_string(event.peer), secured.response.size);
        } else if (secured.kind == SecurityReceiveKind::VERSION_MISMATCH) {
          if (data.session_role == 0) {
            lg::info("[MP-Handshake] Rejected client {} with an incompatible mod version.",
                     enet_peer_endpoint_string(event.peer));
            enet_peer_disconnect_later(event.peer, 0);
            multiplayer_host_peer_release(data, event.peer);
          } else {
            data.required_version = security->remote_version();
            data.client_handshake_started_time = 0;
            data.join_status = (int)MultiplayerStatus::VERSION_MISMATCH;
            data.connection_failure =
                static_cast<int>(MultiplayerConnectionFailure::VERSION_MISMATCH);
            lg::warn("[MP-Handshake] Host {} requires a different mod version.",
                     enet_peer_endpoint_string(event.peer));
          }
        } else if (secured.kind == SecurityReceiveKind::HANDSHAKE && security->authenticated()) {
          lg::info(
              "[MP-Handshake] Secure proof accepted from {}; authenticated peer session is ready.",
              enet_peer_endpoint_string(event.peer));
          if (data.session_role == 0) {
            auto* session = multiplayer_host_peer_find(data, event.peer);
            if (!session || !multiplayer_host_peer_authenticate(data, *session, current_time)) {
              lg::warn("[MP-Handshake] Host is full; rejecting authenticated peer {}.",
                       enet_peer_endpoint_string(event.peer));
              enet_peer_disconnect_later(event.peer, kDisconnectReasonHostFull);
              if (session) {
                multiplayer_host_peer_release(data, *session);
              }
              enet_packet_destroy(event.packet);
              break;
            }
            const PacketSessionWelcome welcome = {
                {PacketType::SESSION_WELCOME, ++data.sequence_num},
                session->player_id,
                data.host_player_id,
                data.session_player_limit,
                session->character};
            if (!mp_send_packet_immediately(data, event.peer,
                                            static_cast<int>(MultiplayerChannel::CONTROL), &welcome,
                                            sizeof(welcome), ENET_PACKET_FLAG_RELIABLE)) {
              enet_peer_disconnect_now(event.peer, 0);
              multiplayer_host_peer_release(data, *session);
              enet_packet_destroy(event.packet);
              break;
            }
            if (!data.host_game_active &&
                data.join_status == static_cast<int>(MultiplayerStatus::CONNECTING)) {
              data.join_status = (int)MultiplayerStatus::CONNECTED_LOBBY;
            }
            lg::info("[MP-Handshake] Assigned player {} to {} (players={}/{}).", session->player_id,
                     enet_peer_endpoint_string(event.peer),
                     multiplayer_host_authenticated_peer_count(data) + 1,
                     data.session_player_limit);
          } else {
            lg::info("[MP-Handshake] Client authenticated; waiting for SESSION_WELCOME.");
          }
          data.client_handshake_started_time = 0;
          data.server_last_receive_time = current_time;
          data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::NONE);
          lg::info("[Multiplayer] Secure handshake peer authenticated.");
        } else if (secured.kind == SecurityReceiveKind::GAMEPLAY) {
          uint32_t sender_player_id = multiplayer_authenticated_sender_player_id(data, event.peer);
          const bool is_pre_assignment_welcome =
              data.session_role == 1 && secured.plaintext.size >= kPacketHeaderWireSize &&
              secured.plaintext.bytes[0] == static_cast<uint8_t>(PacketType::SESSION_WELCOME);
          if (!mp_valid_player_id(sender_player_id) && !is_pre_assignment_welcome) {
            enet_packet_destroy(event.packet);
            break;
          }
          if (is_pre_assignment_welcome) {
            sender_player_id = 0;
          }
          ENetPacket plaintext_packet = {};
          plaintext_packet.data = secured.plaintext.bytes.data();
          plaintext_packet.dataLength = secured.plaintext.size;
          plaintext_packet.flags = event.packet->flags;
          if (data.session_role == 0) {
            if (auto* session = multiplayer_host_peer_find(data, event.peer)) {
              session->last_receive_time = current_time;
            }
          } else {
            data.server_last_receive_time = current_time;
          }
          data.stats.track_recv_bytes(plaintext_packet.data, plaintext_packet.dataLength);
          handle_receive_packet(data, event.peer, sender_player_id, &plaintext_packet, controller,
                                world, bootstrap, current_time, event.channelID);
        } else if (secured.kind == SecurityReceiveKind::REJECTED && data.session_role == 0 &&
                   !multiplayer_peer_is_authenticated(data, event.peer)) {
          lg::warn("[MP-Handshake] Host rejected unauthenticated peer {}.",
                   enet_peer_endpoint_string(event.peer));
          record_authentication_failure(data, event.peer->address.host, current_time);
          enet_peer_disconnect_later(event.peer, kDisconnectReasonAuthenticationRejected);
          multiplayer_host_peer_release(data, event.peer);
        }
        enet_packet_destroy(event.packet);
        break;
      }
      case ENET_EVENT_TYPE_CONNECT:
        if (data.session_role == 1) {
          data.client_handshake_started_time = current_time;
          data.connection_phase = static_cast<int>(MultiplayerConnectionPhase::AUTHENTICATING);
          lg::info(
              "[MP-Reconnect] Client transport connected to {} (local_port={}, peer_state={}, "
              "status={}, reconnect_active={}, attempt={}); awaiting secure authentication.",
              enet_peer_endpoint_string(event.peer), enet_local_port(data.host),
              static_cast<int>(event.peer->state), data.join_status.load(),
              data.reconnect_attempt_active, data.reconnect_attempt_count);
        } else if (data.session_role == 0) {
          const bool address_banned =
              authentication_address_banned(data, event.peer->address.host, current_time);
          if (address_banned) {
            enet_peer_disconnect_later(event.peer, kDisconnectReasonAuthenticationRejected);
            break;
          }
          auto* session = multiplayer_host_peer_allocate(data, event.peer, current_time);
          const bool security_started =
              session &&
              session->security.start_host(enet_local_port(data.host), data.security.room_code()) &&
              session->security.set_local_version(data.local_version);
          MultiplayerDatagram hello;
          const bool hello_created = security_started && session->security.make_server_hello(hello);
          const bool hello_sent =
              hello_created &&
              mp_send_raw_packet_to_peer(event.peer, static_cast<int>(MultiplayerChannel::CONTROL),
                                         hello.bytes.data(), hello.size, ENET_PACKET_FLAG_RELIABLE);
          if (!hello_sent) {
            lg::warn(
                "[MP-Handshake] Failed sending server challenge to {} (created={}, bytes={}, "
                "pending_handshakes={}).",
                enet_peer_endpoint_string(event.peer), hello_created, hello.size,
                pending_handshake_count(data));
            if (session) {
              multiplayer_host_peer_release(data, *session);
            }
            enet_peer_disconnect_later(event.peer, kDisconnectReasonAuthenticationRejected);
          }
          lg::info(
              "[MP-Handshake] Host transport connected from {} (peer_state={}, status={}, "
              "pending_handshakes={}); challenge_sent={}",
              enet_peer_endpoint_string(event.peer), static_cast<int>(event.peer->state),
              data.join_status.load(), pending_handshake_count(data), hello_sent);
        }
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        lg::warn(
            "[MP-Network] Transport disconnected: peer={} reason={} role={} status={} "
            "authenticated={} reconnect_active={}.",
            enet_peer_endpoint_string(event.peer), event.data, data.session_role,
            data.join_status.load(), multiplayer_peer_is_authenticated(data, event.peer),
            data.reconnect_attempt_active);
        if (data.session_role == 0) {
          auto* session = multiplayer_host_peer_find(data, event.peer);
          if (session && session->authenticated && session->identity_ready &&
              mp_valid_player_id(session->player_id)) {
            const uint32_t departed_player_id = session->player_id;
            PacketLeave leave = {{PacketType::EVENT_LEAVE, ++data.sequence_num},
                                 departed_player_id,
                                 MultiplayerLeaveReason::CLIENT_CLOSED};
            mp_clear_player_slot(data, controller, departed_player_id);
            MultiplayerManager::broadcast_except(data, event.peer,
                                                 static_cast<int>(MultiplayerChannel::CONTROL),
                                                 &leave, sizeof(leave), ENET_PACKET_FLAG_RELIABLE);
            lg::info("[Multiplayer] Player {} disconnected; host remains available.",
                     departed_player_id);
          }
          multiplayer_host_peer_release(data, event.peer);
        } else {
          const bool active_session = data.join_status == (int)MultiplayerStatus::CONNECTED_LOBBY ||
                                      data.join_status == (int)MultiplayerStatus::GAME_STARTING ||
                                      data.join_status == (int)MultiplayerStatus::IN_GAME ||
                                      data.join_status == (int)MultiplayerStatus::RECONNECTING ||
                                      data.reconnect_attempt_active;
          for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
            if (player_id != data.local_player_id) {
              mp_clear_player_slot(data, controller, player_id);
            }
          }
          if (event.data == kDisconnectReasonHostFull) {
            data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::HOST_FULL);
            data.join_status = static_cast<int>(MultiplayerStatus::FAILED);
          } else if (event.data == kDisconnectReasonAuthenticationRejected) {
            data.connection_failure =
                static_cast<int>(MultiplayerConnectionFailure::ROOM_CODE_REJECTED);
            data.join_status = static_cast<int>(MultiplayerStatus::FAILED);
          } else if (event.data == kDisconnectReasonHostClosed) {
            lg::warn("[Multiplayer] Host closed the session.");
            multiplayer_cancel_client_reconnect(data);
            data.join_status = (int)MultiplayerStatus::HOST_LEFT;
          } else if (active_session &&
                     data.join_status != (int)MultiplayerStatus::VERSION_MISMATCH) {
            multiplayer_enter_client_reconnect(data, current_time);
          } else {
            lg::warn("[Multiplayer] Host connection closed.");
            if (data.join_status != (int)MultiplayerStatus::VERSION_MISMATCH) {
              data.join_status = (int)MultiplayerStatus::HOST_LEFT;
            }
          }
        }
        break;
      default:
        break;
    }
  }

  if (data.session_role == 0) {
    expire_pending_handshakes(data, current_time);
    for (auto& session : data.host_peer_sessions) {
      if (!session.authenticated || !session.peer || session.last_receive_time == 0 ||
          current_time - session.last_receive_time <= 10000) {
        continue;
      }
      lg::warn("[MP-Network] Player {} timed out; closing only that peer session.",
               session.player_id);
      enet_peer_disconnect_later(session.peer, 0);
      session.last_receive_time = current_time;
    }
  } else if (!data.security.authenticated() && data.client_handshake_started_time != 0 &&
             current_time - data.client_handshake_started_time > 5000) {
    lg::warn(
        "[MP-Handshake] Secure handshake timed out (peer={}, started={}, now={}, status={}, "
        "reconnect_active={}, local_port={}).",
        enet_peer_endpoint_string(data.server_peer), data.client_handshake_started_time,
        current_time, data.join_status.load(), data.reconnect_attempt_active,
        enet_local_port(data.host));
    multiplayer_handle_client_handshake_timeout(data, current_time);
  }
}

MultiplayerData::AuthenticationFailure* authentication_failure_for(MultiplayerData& data,
                                                                   uint32_t address,
                                                                   bool create) {
  for (auto& entry : data.authentication_failures) {
    if (entry.address == address) {
      return &entry;
    }
  }
  if (!create) {
    return nullptr;
  }
  auto& entry = data.authentication_failures[data.next_authentication_failure_slot];
  data.next_authentication_failure_slot =
      (data.next_authentication_failure_slot + 1) % data.authentication_failures.size();
  entry = {};
  entry.address = address;
  return &entry;
}

bool authentication_address_banned(MultiplayerData& data, uint32_t address, uint32_t now) {
  auto* entry = authentication_failure_for(data, address, false);
  return entry && static_cast<int32_t>(entry->banned_until - now) > 0;
}

void record_authentication_failure(MultiplayerData& data, uint32_t address, uint32_t now) {
  auto* entry = authentication_failure_for(data, address, true);
  if (!entry) {
    return;
  }
  if (entry->window_start == 0 || now - entry->window_start > 30000) {
    entry->window_start = now;
    entry->count = 0;
  }
  if (entry->count < UINT8_MAX) {
    ++entry->count;
  }
  if (entry->count >= 3) {
    entry->banned_until = now + 60000;
    entry->window_start = now;
    entry->count = 0;
  }
}

void expire_pending_handshakes(MultiplayerData& data, uint32_t now) {
  for (auto& session : data.host_peer_sessions) {
    if (session.peer && !session.authenticated &&
        static_cast<int32_t>(now - session.handshake_deadline) >= 0) {
      lg::warn(
          "[MP-Handshake] Pending handshake expired for {} (now={}, deadline={}, "
          "pending_handshakes={}).",
          enet_peer_endpoint_string(session.peer), now, session.handshake_deadline,
          pending_handshake_count(data));
      record_authentication_failure(data, session.peer->address.host, now);
      enet_peer_disconnect_later(session.peer, kDisconnectReasonAuthenticationRejected);
      multiplayer_host_peer_release(data, session);
    }
  }
}

template <typename T, typename Fn>
void with_goal_buffer(u32 ptr, const char* label, Fn fn) {
  try {
    T* buffer = goal_ptr<T>(ptr);
    if (buffer) {
      fn(buffer);
    }
  } catch (...) {
    lg::error("[Multiplayer] Exception in {}", label);
  }
}
}  // namespace

static bool try_saved_reconnect(MultiplayerData& data);

int pc_multi_get_role() {
  return multiplayer_data().session_role;
}

u32 pc_multi_get_local_player_id() {
  return multiplayer_data().local_player_id;
}

u32 pc_multi_get_host_player_id() {
  return multiplayer_data().host_player_id;
}

u32 pc_multi_get_local_player_character() {
  return static_cast<u32>(multiplayer_data().local_player_character);
}

int pc_multi_set_local_version(u32 version_ptr) {
  auto& data = multiplayer_data();
  data.local_version.clear();
  data.required_version.clear();
  const char* configured_version = goal_string_data(version_ptr);
  if (!configured_version) {
    return 0;
  }
  size_t length = 0;
  while (length <= kMultiplayerVersionMaxLength && configured_version[length] != '\0') {
    ++length;
  }
  if (length > kMultiplayerVersionMaxLength ||
      !mp_resolve_compatibility_identity(std::string_view(configured_version, length),
                                         build_commit(), data.local_version)) {
    data.local_version.clear();
    return 0;
  }
  return 1;
}

u64 pc_multi_get_local_version() {
  return jak2::make_string_from_c(multiplayer_data().local_version.c_str());
}

u64 pc_multi_get_required_version() {
  const auto& data = multiplayer_data();
  if (data.join_status != (int)MultiplayerStatus::VERSION_MISMATCH) {
    return jak2::make_string_from_c("");
  }
  return jak2::make_string_from_c(data.required_version.c_str());
}

void pc_multi_poll(u32 controller_ptr, u32 world_ptr, u32 bootstrap_ptr) {
  try {
    auto& data = multiplayer_data();
    uint32_t current_time = enet_time_get();
    static uint32_t last_reconnect_heartbeat_time = 0;
    static uint32_t last_reconnect_skip_log_time = 0;
    if (multiplayer_client_reconnect_due(data, current_time)) {
      if (!try_saved_reconnect(data)) {
        if (data.reconnect_invite.empty()) {
          data.join_status = (int)MultiplayerStatus::FAILED;
        } else {
          multiplayer_note_client_reconnect_failed(data, current_time);
        }
      }
    }
    const bool reconnect_phase = data.join_status == (int)MultiplayerStatus::RECONNECTING ||
                                 data.reconnect_attempt_active ||
                                 data.reconnect_waiting_for_bootstrap;
    if (!reconnect_phase) {
      last_reconnect_heartbeat_time = 0;
      last_reconnect_skip_log_time = 0;
    }
    if (!data.initialized || !data.host) {
      if (reconnect_phase && (last_reconnect_skip_log_time == 0 ||
                              current_time - last_reconnect_skip_log_time >= 1000)) {
        lg::warn(
            "[MP-Reconnect] Poll skipped: transport unavailable (initialized={}, host={}, "
            "status={}, attempt_active={}, waiting_bootstrap={}, invite_saved={}).",
            data.initialized, data.host != nullptr, data.join_status.load(),
            data.reconnect_attempt_active, data.reconnect_waiting_for_bootstrap,
            !data.reconnect_invite.empty());
        last_reconnect_skip_log_time = current_time;
      }
      return;
    }

    static uint32_t last_poll_tick = 0;
    bool is_in_game = data.join_status == (int)MultiplayerStatus::IN_GAME;
    bool is_reconnecting = data.join_status == (int)MultiplayerStatus::RECONNECTING;
    if (!is_in_game && !is_reconnecting && current_time - last_poll_tick < 100) {
      return;
    }
    last_poll_tick = current_time;

    auto* controller = goal_ptr<MPPlayerControllerGOAL>(controller_ptr);
    auto* world = goal_ptr<MPWorldSyncStateGOAL>(world_ptr);
    auto* bootstrap = goal_ptr<MPBootstrapSyncStateGOAL>(bootstrap_ptr);
    if (!controller || !world || !bootstrap) {
      if (reconnect_phase && (last_reconnect_skip_log_time == 0 ||
                              current_time - last_reconnect_skip_log_time >= 1000)) {
        lg::warn(
            "[MP-Reconnect] Poll skipped: GOAL sync buffers unavailable (controller=0x{:x}, "
            "world=0x{:x}, bootstrap=0x{:x}, status={}).",
            controller_ptr, world_ptr, bootstrap_ptr, data.join_status.load());
        last_reconnect_skip_log_time = current_time;
      }
      return;
    }

    if (reconnect_phase && (last_reconnect_heartbeat_time == 0 ||
                            current_time - last_reconnect_heartbeat_time >= 1000)) {
      lg::info(
          "[MP-Reconnect] Poll servicing reconnect (status={}, initialized={}, local_port={}, "
          "peer={}, peer_state={}, handshake_started={}, authenticated={}, attempt_active={}, "
          "waiting_bootstrap={}, attempt={}, pending_handshakes={}).",
          data.join_status.load(), data.initialized, enet_local_port(data.host),
          enet_peer_endpoint_string(data.server_peer),
          data.server_peer ? static_cast<int>(data.server_peer->state) : -1,
          data.client_handshake_started_time, data.security.authenticated(),
          data.reconnect_attempt_active, data.reconnect_waiting_for_bootstrap,
          data.reconnect_attempt_count, pending_handshake_count(data));
      last_reconnect_heartbeat_time = current_time;
      last_reconnect_skip_log_time = 0;
    }

    if (data.session_role == 0 && mp_valid_player_id(controller->local_player_id)) {
      data.local_player_id = controller->local_player_id;
    }
    if (data.session_role == 0 && mp_valid_player_id(controller->host_player_id)) {
      data.host_player_id = controller->host_player_id;
    }
    poll_network(data, controller, world, bootstrap);
    current_time = enet_time_get();
    multiplayer_cleanup_stale_sync(data, current_time);
    multiplayer_update_receive_timeout(data, current_time);
  } catch (...) {
    lg::error("[Multiplayer] Exception in pc_multi_poll");
  }
}

int pc_multi_flush_packet_window() {
  try {
    auto& data = multiplayer_data();
    if (data.initialized && data.host) {
      mp_flush_packet_window(data);
      return 1;
    }
  } catch (...) {
    lg::error("[Multiplayer] Exception in pc_multi_flush_packet_window");
  }
  return 0;
}

void pc_multi_send_sync(u32 controller_ptr, u32 world_ptr, u32 bootstrap_ptr) {
  if (multiplayer_debug_receive_stopped()) {
    return;
  }
  auto* controller = goal_ptr<MPPlayerControllerGOAL>(controller_ptr);
  auto* world = goal_ptr<MPWorldSyncStateGOAL>(world_ptr);
  auto* bootstrap = goal_ptr<MPBootstrapSyncStateGOAL>(bootstrap_ptr);
  auto& data = multiplayer_data();
  if (controller && world && bootstrap && data.initialized && data.host) {
    mp_send_player_sync(data, controller, world, bootstrap);
  }
}

void pc_multi_receive_sync(u32 controller_ptr, u32 world_ptr, u32 bootstrap_ptr) {
  auto* controller = goal_ptr<MPPlayerControllerGOAL>(controller_ptr);
  auto* world = goal_ptr<MPWorldSyncStateGOAL>(world_ptr);
  auto* bootstrap = goal_ptr<MPBootstrapSyncStateGOAL>(bootstrap_ptr);
  auto& data = multiplayer_data();
  if (controller && world && bootstrap && data.initialized && data.host) {
    mp_receive_player_sync(data, controller, world, bootstrap);
  }
}

void pc_multi_send_events(u32 event_ptr) {
  if (multiplayer_debug_receive_stopped()) {
    return;
  }
  with_goal_buffer<MPEventBufferGOAL>(event_ptr, "pc_multi_send_events", [](auto* events) {
    auto& data = multiplayer_data();
    if (data.initialized) {
      mp_send_game_events(data, events);
    }
  });
}

void pc_multi_receive_events(u32 event_ptr) {
  with_goal_buffer<MPEventBufferGOAL>(event_ptr, "pc_multi_receive_events", [](auto* events) {
    auto& data = multiplayer_data();
    if (data.initialized) {
      mp_receive_game_events(data, events);
    }
  });
}

void pc_multi_send_enemies(u32 buffer_ptr) {
  with_goal_buffer<MPEnemySyncBufferGOAL>(buffer_ptr, "pc_multi_send_enemies", [](auto* buffer) {
    auto& data = multiplayer_data();
    if (data.initialized) {
      mp_send_enemy_sync(data, buffer);
    }
  });
}

void pc_multi_receive_enemies(u32 buffer_ptr) {
  with_goal_buffer<MPEnemySyncBufferGOAL>(buffer_ptr, "pc_multi_receive_enemies", [](auto* buffer) {
    auto& data = multiplayer_data();
    if (data.initialized) {
      mp_receive_enemy_sync(data, buffer);
    }
  });
}

void pc_multi_send_traffic(u32 buffer_ptr) {
  with_goal_buffer<MPTrafficSyncBufferGOAL>(buffer_ptr, "pc_multi_send_traffic", [](auto* buffer) {
    auto& data = multiplayer_data();
    if (data.initialized) {
      mp_send_traffic_sync(data, buffer);
    }
  });
}

void pc_multi_receive_traffic(u32 buffer_ptr) {
  with_goal_buffer<MPTrafficSyncBufferGOAL>(buffer_ptr, "pc_multi_receive_traffic",
                                            [](auto* buffer) {
                                              auto& data = multiplayer_data();
                                              if (data.initialized) {
                                                mp_receive_traffic_sync(data, buffer);
                                              }
                                            });
}

void pc_multi_clear_remote_traffic() {
  try {
    multiplayer_reset_remote_traffic_buffers(multiplayer_data());
  } catch (...) {
    lg::error("[Multiplayer] Exception in pc_multi_clear_remote_traffic");
  }
}

u32 pc_multi_publish_traffic_authority_map(u32 authority_map_ptr) {
  try {
    auto& data = multiplayer_data();
    if (data.session_role != 0) {
      return data.traffic_authority_revision;
    }
    const auto* map = goal_byte_array_ptr(authority_map_ptr, kMPMaxPlayers);
    if (!map) {
      lg::error("[Multiplayer] Invalid traffic authority map pointer in publish.");
      return data.traffic_authority_revision;
    }
    return mp_set_traffic_authority_map(data, map);
  } catch (...) {
    lg::error("[Multiplayer] Exception in pc_multi_publish_traffic_authority_map");
    return 0;
  }
}

u32 pc_multi_read_traffic_authority_map(u32 authority_map_ptr) {
  try {
    auto& data = multiplayer_data();
    auto* map = goal_byte_array_ptr_mut(authority_map_ptr, kMPMaxPlayers);
    if (!map) {
      return 0;
    }
    memcpy(map, data.traffic_authority_map.data(), kMPMaxPlayers);
    return data.traffic_authority_revision;
  } catch (...) {
    lg::error("[Multiplayer] Exception in pc_multi_read_traffic_authority_map");
    return 0;
  }
}

void pc_multi_set_selected_traffic_authority(u32 selected_authority) {
  try {
    auto& data = multiplayer_data();
    mp_set_selected_traffic_authority(data, selected_authority);
  } catch (...) {
    lg::error("[Multiplayer] Exception in pc_multi_set_selected_traffic_authority");
  }
}

void pc_multi_send_palace_squid(u32 buffer_ptr) {
  with_goal_buffer<MPPalaceSquidSyncBufferGOAL>(buffer_ptr, "pc_multi_send_palace_squid",
                                                [](auto* buffer) {
                                                  auto& data = multiplayer_data();
                                                  if (data.initialized) {
                                                    mp_send_palace_squid_sync(data, buffer);
                                                  }
                                                });
}

void pc_multi_receive_palace_squid(u32 buffer_ptr) {
  with_goal_buffer<MPPalaceSquidSyncBufferGOAL>(buffer_ptr, "pc_multi_receive_palace_squid",
                                                [](auto* buffer) {
                                                  auto& data = multiplayer_data();
                                                  if (data.initialized) {
                                                    mp_receive_palace_squid_sync(data, buffer);
                                                  }
                                                });
}

void pc_multi_send_widow(u32 buffer_ptr) {
  with_goal_buffer<MPWidowSyncBufferGOAL>(buffer_ptr, "pc_multi_send_widow", [](auto* buffer) {
    auto& data = multiplayer_data();
    if (data.initialized) {
      mp_send_widow_sync(data, buffer);
    }
  });
}

void pc_multi_receive_widow(u32 buffer_ptr) {
  with_goal_buffer<MPWidowSyncBufferGOAL>(buffer_ptr, "pc_multi_receive_widow", [](auto* buffer) {
    auto& data = multiplayer_data();
    if (data.initialized) {
      mp_receive_widow_sync(data, buffer);
    }
  });
}

void pc_multi_send_airlock_state(u32 buffer_ptr) {
  if (multiplayer_debug_receive_stopped()) {
    return;
  }
  with_goal_buffer<MPAirlockSyncBufferGOAL>(buffer_ptr, "pc_multi_send_airlock_state",
                                            [](auto* buffer) {
                                              auto& data = multiplayer_data();
                                              if (data.initialized) {
                                                mp_send_airlock_sync(data, buffer);
                                              }
                                            });
}

void pc_multi_receive_airlock_state(u32 buffer_ptr) {
  with_goal_buffer<MPAirlockSyncBufferGOAL>(buffer_ptr, "pc_multi_receive_airlock_state",
                                            [](auto* buffer) {
                                              auto& data = multiplayer_data();
                                              if (data.initialized) {
                                                mp_receive_airlock_sync(data, buffer);
                                              }
                                            });
}

u64 pc_multi_get_enemy_sync_time() {
  return multiplayer_data().last_enemy_sync_time;
}

u64 pc_multi_get_vehicle_sync_time(u32 net_id) {
  if (net_id == 0) {
    return 0;
  }

  const auto& data = multiplayer_data();
  for (const auto& player : data.player_states) {
    if (player.state_ready && player.veh_state.net_id == net_id) {
      return player.receive_tick;
    }
  }

  for (uint32_t slot = 0; slot < MAX_VEHICLE_SYNC_COUNT; ++slot) {
    if (data.traffic_buffer.vehicles[slot].net_id == net_id) {
      return data.veh_last_updated[slot];
    }
  }
  return 0;
}

void pc_multi_disconnect() {
  MultiplayerManager::disconnect(multiplayer_data());
}

static bool read_goal_player_character_config(
    u32 character_config_ptr,
    std::array<MPPlayerCharacter, kMPMaxPlayers>& characters) {
  const auto* goal_config = goal_ptr<MPPlayerCharacterConfigGOAL>(character_config_ptr);
  if (!goal_config) {
    lg::error("[Multiplayer] Invalid GOAL player character configuration pointer 0x{:x}.",
              character_config_ptr);
    return false;
  }
  for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    const auto character = goal_config->characters[player_id];
    if (!mp_valid_player_character(character)) {
      lg::error("[Multiplayer] Invalid character {} configured for player {}.",
                static_cast<uint32_t>(character), player_id);
      return false;
    }
    characters[player_id] = character;
  }
  return true;
}

void pc_multi_setup_host(u32 player_limit, u32 character_config_ptr) {
  std::array<MPPlayerCharacter, kMPMaxPlayers> characters = {};
  if (read_goal_player_character_config(character_config_ptr, characters)) {
    MultiplayerManager::setup_host(multiplayer_data(), false, player_limit, characters);
  }
}

void pc_multi_setup_internet_host(u32 player_limit, u32 character_config_ptr) {
  std::array<MPPlayerCharacter, kMPMaxPlayers> characters = {};
  if (read_goal_player_character_config(character_config_ptr, characters)) {
    MultiplayerManager::setup_host(multiplayer_data(), true, player_limit, characters);
  }
}

void pc_multi_setup_client(u32 ip_ptr, u32 port) {
  const char* invite = goal_string_data(ip_ptr);
  if (!invite || !invite[0]) {
    lg::warn("[Multiplayer] Ignoring setup-client with empty invite string.");
    return;
  }
  auto& data = multiplayer_data();
  MultiplayerManager::disconnect(data);
  data.connection_phase = static_cast<int>(MultiplayerConnectionPhase::VALIDATING);
  data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::NONE);
  std::string host;
  uint16_t invite_port = 0;
  if (data.security.start_client(invite, host, invite_port)) {
    data.reconnect_invite = invite;
    MultiplayerManager::setup_client(data, host.c_str(), invite_port);
    return;
  }
  if (port > 0 && port <= (std::numeric_limits<uint16_t>::max)() &&
      MultiplayerScanner::start_direct_search(data, invite, static_cast<uint16_t>(port))) {
    lg::info("[Multiplayer] Retrieving the host credential over LAN discovery.");
    return;
  }
  lg::warn("[Multiplayer] Rejected invalid or legacy multiplayer connection argument.");
  data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::INVALID_INVITE);
  data.join_status = (int)MultiplayerStatus::FAILED;
}

int64_t pc_multi_get_status() {
  return static_cast<int64_t>(MultiplayerScanner::get_status(multiplayer_data()));
}

void pc_multi_set_status(int status) {
  multiplayer_set_status(multiplayer_data(), status);
}

void pc_multi_request_bootstrap() {
  auto& data = multiplayer_data();
  if (data.session_role == 0 && data.join_status == (int)MultiplayerStatus::IN_GAME) {
    multiplayer_request_bootstrap(data);
    lg::info("[Multiplayer] Bootstrap requested by GOAL.");
  }
}

void pc_multi_request_full_sync() {
  // Keep the old GOAL symbol as a forwarding compatibility alias.
  pc_multi_request_bootstrap();
}

void pc_multi_stop_search() {
  MultiplayerScanner::stop_search(multiplayer_data());
}

void pc_multi_start_search() {
  multiplayer_clear_direct_connect_draft(multiplayer_data());
  MultiplayerScanner::start_search(multiplayer_data());
}

u64 pc_multi_get_command_line_arg(u32 str_ptr) {
  const char* arg_name = goal_string_data(str_ptr);
  if (!arg_name) {
    return s7.offset;
  }
  for (int i = 1; i < g_argc; i++) {
    if (g_argv[i] && strcmp(g_argv[i], arg_name) == 0) {
      return jak2::make_string_from_c(i + 1 < g_argc ? g_argv[i + 1] : "");
    }
  }
  return s7.offset;
}

static void connect_private_invite(MultiplayerData& data,
                                   std::string& invite,
                                   bool reconnect_attempt) {
  lg::info(
      "[MP-Reconnect] Connection request (reconnect={}, status={}, attempt_active={}, "
      "attempt_count={}, invite_saved={}).",
      reconnect_attempt, data.join_status.load(), data.reconnect_attempt_active,
      data.reconnect_attempt_count, !data.reconnect_invite.empty());
  MultiplayerManager::disconnect(data, reconnect_attempt);
  std::string host;
  uint16_t port = 0;
  const bool valid = data.security.start_client(invite, host, port);
  if (valid) {
    lg::info("[MP-Reconnect] Parsed connection target {}:{} (reconnect={}).", host, port,
             reconnect_attempt);
    data.reconnect_invite = invite;
    data.connection_phase = static_cast<int>(MultiplayerConnectionPhase::CONTACTING_HOST);
  } else {
    lg::warn("[MP-Reconnect] Failed to parse connection invite (reconnect={}).", reconnect_attempt);
  }
  mp_secure_clear_string(invite);
  if (!valid) {
    data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::INVALID_INVITE);
    data.join_status = (int)MultiplayerStatus::FAILED;
    return;
  }
  MultiplayerManager::setup_client(data, host.c_str(), port);
}

static bool try_saved_reconnect(MultiplayerData& data) {
  if (data.reconnect_invite.empty()) {
    return false;
  }

  std::string invite = data.reconnect_invite;
  connect_private_invite(data, invite, true);
  if (!data.initialized || data.session_role != 1) {
    return false;
  }

  multiplayer_note_client_reconnect_attempt_started(data);
  return true;
}

int pc_multi_reconnect() {
  auto& data = multiplayer_data();
  lg::info(
      "[MP-Reconnect] Manual reconnect requested (status={}, attempt_active={}, "
      "waiting_bootstrap={}, attempt_count={}, invite_saved={}).",
      data.join_status.load(), data.reconnect_attempt_active, data.reconnect_waiting_for_bootstrap,
      data.reconnect_attempt_count, !data.reconnect_invite.empty());
  if (data.reconnect_invite.empty()) {
    lg::warn("[Multiplayer] Reconnect requested without a saved client invite.");
    data.join_status = (int)MultiplayerStatus::FAILED;
    return 0;
  }

  if (try_saved_reconnect(data)) {
    lg::info("[Multiplayer] Client reconnect attempt started.");
    return 1;
  }

  lg::warn("[Multiplayer] Client reconnect attempt failed to start.");
  return 0;
}

void pc_multi_clear_direct_connect() {
  mp_direct_connect_clear(multiplayer_data());
}

void pc_multi_reset_direct_connect() {
  mp_direct_connect_reset(multiplayer_data());
}

u64 pc_multi_get_direct_field(int field) {
  const std::string display = mp_direct_connect_display(multiplayer_data(), field);
  return jak2::make_string_from_c(display.c_str());
}

int pc_multi_edit_direct_field(int field, u32 key) {
  return mp_direct_connect_edit(multiplayer_data(), field, key);
}

int pc_multi_direct_connect_ready() {
  return mp_direct_connect_ready(multiplayer_data()) ? 1 : 0;
}

int pc_multi_connect_direct() {
  return mp_direct_connect_start(multiplayer_data()) ? 1 : 0;
}

u64 pc_multi_get_preference_field(int field) {
  const std::string display = mp_multiplayer_preference_display(field);
  return jak2::make_string_from_c(display.c_str());
}

u64 pc_multi_get_player_name() {
  return jak2::make_string_from_c(mp_multiplayer_preferences().player_name.c_str());
}

int pc_multi_edit_preference_field(int field, u32 key) {
  return mp_edit_multiplayer_preference(field, key);
}

int pc_multi_commit_preference_field(int field) {
  return mp_commit_multiplayer_preference(field) ? 1 : 0;
}

void pc_multi_discard_preference_edits() {
  mp_discard_multiplayer_preference_edits();
}

int pc_multi_get_automatic_port_mapping() {
  return mp_multiplayer_preferences().automatic_port_mapping ? 1 : 0;
}

void pc_multi_set_automatic_port_mapping(int enabled) {
  mp_set_automatic_port_mapping(enabled != 0);
}

void pc_multi_reset_preferences() {
  mp_reset_multiplayer_preferences();
}

u32 pc_multi_get_preference_player_limit() {
  return mp_get_session_player_limit_preference();
}

void pc_multi_set_preference_player_limit(u32 limit) {
  mp_set_session_player_limit_preference(limit);
}

u32 pc_multi_get_preference_player_character(u32 player_id) {
  return mp_get_session_player_character_preference(player_id);
}

void pc_multi_set_preference_player_character(u32 player_id, u32 character) {
  mp_set_session_player_character_preference(player_id, character);
}

int pc_multi_is_lobby_host() {
  const auto& data = multiplayer_data();
  return data.session_role == 0 ? 1 : 0;
}

u32 pc_multi_get_session_player_limit() {
  const auto& data = multiplayer_data();
  return data.session_player_limit;
}

int pc_multi_lobby_start_game() {
  auto& data = multiplayer_data();
  if (data.session_role != 0 || !data.host) {
    return 0;
  }
  PacketLobbyAction action = {};
  action.header = {PacketType::LOBBY_ACTION, ++data.sequence_num};
  action.player_id = data.local_player_id;
  action.action_type = static_cast<uint8_t>(MPLobbyActionType::START_GAME);
  action.value = 0;
  MultiplayerManager::broadcast(data, static_cast<int>(MultiplayerChannel::CONTROL), action,
                                ENET_PACKET_FLAG_RELIABLE);
  data.join_status = static_cast<int>(MultiplayerStatus::GAME_STARTING);
  lg::info("[MP-Lobby] Host triggered game start.");
  return 1;
}

int pc_multi_lobby_set_character(u32 character) {
  auto& data = multiplayer_data();
  const auto char_enum = static_cast<MPPlayerCharacter>(character);
  if (!mp_valid_player_character(char_enum)) {
    return 0;
  }
  const uint32_t local_id = data.local_player_id;
  if (!mp_valid_player_id(local_id)) {
    return 0;
  }
  data.local_player_character = char_enum;
  if (data.session_role == 0) {
    if (local_id < data.session_player_characters.size()) {
      data.session_player_characters[local_id] = char_enum;
    }
  }
  PacketLobbyAction action = {};
  action.header = {PacketType::LOBBY_ACTION, ++data.sequence_num};
  action.player_id = local_id;
  action.action_type = static_cast<uint8_t>(MPLobbyActionType::SET_CHARACTER);
  action.value = character;
  MultiplayerManager::broadcast(data, static_cast<int>(MultiplayerChannel::CONTROL), action,
                                ENET_PACKET_FLAG_RELIABLE);
  lg::info("[MP-Lobby] Local player {} broadcasted character choice {}.", local_id, character);
  return 1;
}

u32 pc_multi_get_player_color() {
  return mp_multiplayer_preferences()
      .player_appearance.colors[mp_player_appearance_group_index(MPPlayerAppearanceGroup::PRIMARY)];
}

int pc_multi_get_player_appearance(u32 appearance_ptr) {
  auto* goal_appearance = goal_ptr<MPPlayerAppearanceGOAL>(appearance_ptr);
  if (!goal_appearance) {
    return 0;
  }
  mp_copy_player_appearance_to_goal(mp_multiplayer_preferences().player_appearance,
                                    *goal_appearance);
  return 1;
}

int pc_multi_lobby_set_appearance(u32 appearance_ptr) {
  const auto* goal_appearance = goal_ptr<MPPlayerAppearanceGOAL>(appearance_ptr);
  if (!goal_appearance) {
    return 0;
  }
  const MPPlayerAppearance appearance = mp_player_appearance_from_goal(*goal_appearance);
  auto& data = multiplayer_data();
  if (!mp_set_player_appearance(appearance)) {
    return 0;
  }

  const uint32_t local_id = data.local_player_id;
  if (data.join_status != static_cast<int>(MultiplayerStatus::CONNECTED_LOBBY) ||
      !mp_valid_player_id(local_id)) {
    return 1;
  }

  data.player_states[local_id].appearance = appearance;
  PacketLobbyAction action = {};
  action.header = {PacketType::LOBBY_ACTION, ++data.sequence_num};
  action.player_id = local_id;
  action.action_type = static_cast<uint8_t>(MPLobbyActionType::SET_APPEARANCE);
  action.appearance = appearance;
  MultiplayerManager::broadcast(data, static_cast<int>(MultiplayerChannel::CONTROL), action,
                                ENET_PACKET_FLAG_RELIABLE);
  lg::info("[MP-Lobby] Local player {} broadcasted its complete appearance.", local_id);
  return 1;
}

int64_t pc_multi_get_host_setup_status() {
  return static_cast<int64_t>(multiplayer_data().host_setup_status.load());
}

int pc_multi_get_host_port() {
  const auto& data = multiplayer_data();
  return data.host ? enet_local_port(data.host) : mp_resolved_host_port();
}

int pc_multi_get_connection_phase() {
  return multiplayer_data().connection_phase;
}

int pc_multi_get_connection_failure() {
  return multiplayer_data().connection_failure;
}

void pc_multi_connect_found_host() {
  auto& data = multiplayer_data();
  std::string invite;
  {
    std::lock_guard<std::mutex> lock(data.discovery_result_mutex);
    invite.swap(data.found_ip);
  }
  if (invite.empty()) {
    data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::HOST_UNREACHABLE);
    data.join_status = (int)MultiplayerStatus::FAILED;
    return;
  }
  connect_private_invite(data, invite, false);
}

static std::string current_host_copy_payload(MultiplayerData& data) {
  const auto mode = multiplayer_host_copy_mode(data);
  if (mode == MultiplayerHostCopyMode::ROOM_CODE) {
    return data.security.room_code();
  }
  if (mode != MultiplayerHostCopyMode::INVITE) {
    return {};
  }
  std::lock_guard<std::mutex> lock(data.port_mapping_mutex);
  if (data.port_mapping_state != MPPortMappingState::READY ||
      !mp_is_public_ipv4(data.port_mapping_external_ip)) {
    return {};
  }
  return data.security.invite_for_address(data.port_mapping_external_ip);
}

int pc_multi_get_host_copy_mode() {
  return static_cast<int>(multiplayer_host_copy_mode(multiplayer_data()));
}

int pc_multi_copy_host_access() {
  std::string payload = current_host_copy_payload(multiplayer_data());
  if (payload.empty()) {
    return 0;
  }
  const bool copied = SDL_SetClipboardText(payload.c_str());
  mp_secure_clear_string(payload);
  return copied ? 1 : 0;
}

void pc_multi_clear_staged_invite() {
  auto& data = multiplayer_data();
  mp_secure_clear_string(data.staged_invite);
  data.staged_invite_status = 0;
}

int pc_multi_stage_clipboard_invite() {
  auto& data = multiplayer_data();
  multiplayer_clear_direct_connect_draft(data);
  pc_multi_clear_staged_invite();
  char* clipboard_text = SDL_GetClipboardText();
  if (!clipboard_text) {
    data.staged_invite_status = -1;
    data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::INVALID_INVITE);
    return 0;
  }

  constexpr size_t kMaximumInviteLength = 37;
  size_t length = 0;
  while (length <= kMaximumInviteLength && clipboard_text[length] != '\0') {
    ++length;
  }
  bool valid = length <= kMaximumInviteLength;
  std::string candidate;
  if (valid) {
    candidate.assign(clipboard_text, length);
    MultiplayerSecurity validator;
    std::string host;
    uint16_t port = 0;
    valid = validator.start_client(candidate, host, port);
  }
  SDL_free(clipboard_text);

  if (!valid) {
    mp_secure_clear_string(candidate);
    data.staged_invite_status = -1;
    data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::INVALID_INVITE);
    return 0;
  }
  data.staged_invite.swap(candidate);
  data.staged_invite_status = 1;
  data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::NONE);
  return 1;
}

int pc_multi_get_staged_invite_status() {
  return multiplayer_data().staged_invite_status;
}

void pc_multi_connect_staged_invite() {
  auto& data = multiplayer_data();
  if (data.staged_invite_status != 1 || data.staged_invite.empty()) {
    data.join_status = (int)MultiplayerStatus::FAILED;
    return;
  }

  std::string invite;
  invite.swap(data.staged_invite);
  data.staged_invite_status = 0;
  connect_private_invite(data, invite, false);
}

void pc_multi_debug_stop_receive(u32 val) {
  multiplayer_set_debug_receive_stopped(val != 0);
}

u64 pc_multi_get_ticks() {
  return enet_time_get();
}

int pc_multi_get_player_ping(u32 player_id) {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  if (data.session_role == 0) {
    if (player_id == data.local_player_id) {
      return 0;
    }
    auto* session = multiplayer_host_peer_for_player_id(data, player_id);
    if (session && session->peer && session->authenticated &&
        session->peer->state == ENET_PEER_STATE_CONNECTED &&
        multiplayer_enet_rtt_sample_valid(*session->peer)) {
      return session->peer->roundTripTime;
    }
    return 0;
  }

  if (data.server_peer && data.server_peer->state == ENET_PEER_STATE_CONNECTED &&
      multiplayer_enet_rtt_sample_valid(*data.server_peer)) {
    return data.server_peer->roundTripTime;
  }
  return 0;
}

int pc_multi_get_ping() {
  auto& data = multiplayer_data();
  return pc_multi_get_player_ping(data.local_player_id);
}

int pc_multi_get_packet_loss() {
  return static_cast<int>(pc_multi_get_packet_loss_percent());
}

int pc_multi_get_ping_valid() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  if (data.server_peer) {
    return data.server_peer->state == ENET_PEER_STATE_CONNECTED &&
                   multiplayer_enet_rtt_sample_valid(*data.server_peer)
               ? 1
               : 0;
  }

  for (size_t i = 0; i < data.host->peerCount; i++) {
    if (multiplayer_peer_is_authenticated(data, &data.host->peers[i]) &&
        data.host->peers[i].state == ENET_PEER_STATE_CONNECTED &&
        multiplayer_enet_rtt_sample_valid(data.host->peers[i])) {
      return 1;
    }
  }
  return 0;
}

float pc_multi_get_packet_loss_percent() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0.0f;
  }
  if (data.server_peer) {
    if (data.server_peer->state != ENET_PEER_STATE_CONNECTED) {
      return 0.0f;
    }
    return multiplayer_enet_ratio_to_percent(data.server_peer->packetLoss);
  }

  uint64_t total = 0;
  uint32_t count = 0;
  for (size_t i = 0; i < data.host->peerCount; i++) {
    if (multiplayer_peer_is_authenticated(data, &data.host->peers[i]) &&
        data.host->peers[i].state == ENET_PEER_STATE_CONNECTED) {
      total += data.host->peers[i].packetLoss;
      count++;
    }
  }
  return count > 0 ? multiplayer_enet_ratio_to_percent(static_cast<uint32_t>(total / count)) : 0.0f;
}

float pc_multi_get_packet_loss_variance_percent() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0.0f;
  }
  if (data.server_peer) {
    if (data.server_peer->state != ENET_PEER_STATE_CONNECTED) {
      return 0.0f;
    }
    return multiplayer_enet_ratio_to_percent(data.server_peer->packetLossVariance);
  }

  uint64_t total = 0;
  uint32_t count = 0;
  for (size_t i = 0; i < data.host->peerCount; i++) {
    if (multiplayer_peer_is_authenticated(data, &data.host->peers[i]) &&
        data.host->peers[i].state == ENET_PEER_STATE_CONNECTED) {
      total += data.host->peers[i].packetLossVariance;
      count++;
    }
  }
  return count > 0 ? multiplayer_enet_ratio_to_percent(static_cast<uint32_t>(total / count)) : 0.0f;
}

int pc_multi_get_ping_variance() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  if (data.server_peer && data.server_peer->state == ENET_PEER_STATE_CONNECTED &&
      multiplayer_enet_rtt_sample_valid(*data.server_peer)) {
    return data.server_peer->roundTripTimeVariance;
  }

  uint64_t total = 0;
  uint32_t count = 0;
  for (size_t i = 0; i < data.host->peerCount; i++) {
    if (multiplayer_peer_is_authenticated(data, &data.host->peers[i]) &&
        data.host->peers[i].state == ENET_PEER_STATE_CONNECTED &&
        multiplayer_enet_rtt_sample_valid(data.host->peers[i])) {
      total += data.host->peers[i].roundTripTimeVariance;
      count++;
    }
  }
  return count > 0 ? (total / count) : 0;
}

int pc_multi_get_total_sent_bytes() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  return data.host->totalSentData;
}

int pc_multi_get_total_received_bytes() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  return data.host->totalReceivedData;
}

int pc_multi_get_total_sent_packets() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  return data.host->totalSentPackets;
}

int pc_multi_get_total_received_packets() {
  auto& data = multiplayer_data();
  if (!data.host) {
    return 0;
  }
  return data.host->totalReceivedPackets;
}

int pc_multi_get_send_rate() {
  auto& data = multiplayer_data();
  return data.stats.send_rate_bytes_per_sec;
}

int pc_multi_get_recv_rate() {
  auto& data = multiplayer_data();
  return data.stats.recv_rate_bytes_per_sec;
}

int pc_multi_get_send_packet_rate() {
  auto& data = multiplayer_data();
  return data.stats.send_rate_packets_per_sec;
}

int pc_multi_get_recv_packet_rate() {
  auto& data = multiplayer_data();
  return data.stats.recv_rate_packets_per_sec;
}

u64 pc_multi_get_wire_total_sent_bytes() {
  return multiplayer_data().stats.wire_total_sent_bytes;
}

u64 pc_multi_get_wire_total_received_bytes() {
  return multiplayer_data().stats.wire_total_recv_bytes;
}

u64 pc_multi_get_wire_total_sent_packets() {
  return multiplayer_data().stats.wire_total_sent_packets;
}

u64 pc_multi_get_wire_total_received_packets() {
  return multiplayer_data().stats.wire_total_recv_packets;
}

int pc_multi_get_type_send_rate(int type) {
  if (!multiplayer_stats_valid_packet_type(type))
    return 0;
  return multiplayer_data().stats.send_rate_by_type[type];
}

int pc_multi_get_type_recv_rate(int type) {
  if (!multiplayer_stats_valid_packet_type(type))
    return 0;
  return multiplayer_data().stats.recv_rate_by_type[type];
}

int pc_multi_get_type_total_sent(int type) {
  if (!multiplayer_stats_valid_packet_type(type))
    return 0;
  return (int)multiplayer_data().stats.sent_bytes_by_type[type];
}

int pc_multi_get_type_total_recv(int type) {
  if (!multiplayer_stats_valid_packet_type(type))
    return 0;
  return (int)multiplayer_data().stats.recv_bytes_by_type[type];
}

int pc_multi_get_type_send_packet_rate(int type) {
  if (!multiplayer_stats_valid_packet_type(type))
    return 0;
  return multiplayer_data().stats.send_packet_rate_by_type[type];
}

int pc_multi_get_type_recv_packet_rate(int type) {
  if (!multiplayer_stats_valid_packet_type(type))
    return 0;
  return multiplayer_data().stats.recv_packet_rate_by_type[type];
}

u64 pc_multi_get_type_total_sent_packets(int type) {
  if (!multiplayer_stats_valid_packet_type(type))
    return 0;
  return multiplayer_data().stats.sent_packets_by_type[type];
}

u64 pc_multi_get_type_total_received_packets(int type) {
  if (!multiplayer_stats_valid_packet_type(type))
    return 0;
  return multiplayer_data().stats.recv_packets_by_type[type];
}

void init_multiplayer_pc_port() {
  mp_load_multiplayer_preferences();
  jak2::make_function_symbol_from_c("pc-multi-set-local-version",
                                    (void*)pc_multi_set_local_version);
  jak2::make_function_symbol_from_c("pc-multi-get-local-version",
                                    (void*)pc_multi_get_local_version);
  jak2::make_function_symbol_from_c("pc-multi-get-required-version",
                                    (void*)pc_multi_get_required_version);
  jak2::make_function_symbol_from_c("pc-multi-setup-host", (void*)pc_multi_setup_host);
  jak2::make_function_symbol_from_c("pc-multi-setup-internet-host",
                                    (void*)pc_multi_setup_internet_host);
  jak2::make_function_symbol_from_c("pc-multi-setup-client", (void*)pc_multi_setup_client);
  jak2::make_function_symbol_from_c("pc-multi-get-status", (void*)pc_multi_get_status);
  jak2::make_function_symbol_from_c("pc-multi-set-status", (void*)pc_multi_set_status);
  jak2::make_function_symbol_from_c("pc-multi-request-bootstrap",
                                    (void*)pc_multi_request_bootstrap);
  jak2::make_function_symbol_from_c("pc-multi-request-full-sync",
                                    (void*)pc_multi_request_full_sync);
  jak2::make_function_symbol_from_c("pc-multi-stop-search", (void*)pc_multi_stop_search);
  jak2::make_function_symbol_from_c("pc-multi-start-search", (void*)pc_multi_start_search);
  jak2::make_function_symbol_from_c("pc-multi-connect-found-host",
                                    (void*)pc_multi_connect_found_host);
  jak2::make_function_symbol_from_c("pc-multi-get-host-copy-mode",
                                    (void*)pc_multi_get_host_copy_mode);
  jak2::make_function_symbol_from_c("pc-multi-copy-host-access", (void*)pc_multi_copy_host_access);
  jak2::make_function_symbol_from_c("pc-multi-stage-clipboard-invite",
                                    (void*)pc_multi_stage_clipboard_invite);
  jak2::make_function_symbol_from_c("pc-multi-get-staged-invite-status",
                                    (void*)pc_multi_get_staged_invite_status);
  jak2::make_function_symbol_from_c("pc-multi-connect-staged-invite",
                                    (void*)pc_multi_connect_staged_invite);
  jak2::make_function_symbol_from_c("pc-multi-clear-staged-invite",
                                    (void*)pc_multi_clear_staged_invite);
  jak2::make_function_symbol_from_c("pc-multi-clear-direct-connect",
                                    (void*)pc_multi_clear_direct_connect);
  jak2::make_function_symbol_from_c("pc-multi-reset-direct-connect",
                                    (void*)pc_multi_reset_direct_connect);
  jak2::make_function_symbol_from_c("pc-multi-get-direct-field", (void*)pc_multi_get_direct_field);
  jak2::make_function_symbol_from_c("pc-multi-edit-direct-field",
                                    (void*)pc_multi_edit_direct_field);
  jak2::make_function_symbol_from_c("pc-multi-direct-connect-ready",
                                    (void*)pc_multi_direct_connect_ready);
  jak2::make_function_symbol_from_c("pc-multi-connect-direct", (void*)pc_multi_connect_direct);
  jak2::make_function_symbol_from_c("pc-multi-get-preference-field",
                                    (void*)pc_multi_get_preference_field);
  jak2::make_function_symbol_from_c("pc-multi-get-player-name", (void*)pc_multi_get_player_name);
  jak2::make_function_symbol_from_c("pc-multi-edit-preference-field",
                                    (void*)pc_multi_edit_preference_field);
  jak2::make_function_symbol_from_c("pc-multi-commit-preference-field",
                                    (void*)pc_multi_commit_preference_field);
  jak2::make_function_symbol_from_c("pc-multi-discard-preference-edits",
                                    (void*)pc_multi_discard_preference_edits);
  jak2::make_function_symbol_from_c("pc-multi-get-automatic-port-mapping",
                                    (void*)pc_multi_get_automatic_port_mapping);
  jak2::make_function_symbol_from_c("pc-multi-set-automatic-port-mapping",
                                    (void*)pc_multi_set_automatic_port_mapping);
  jak2::make_function_symbol_from_c("pc-multi-reset-preferences",
                                    (void*)pc_multi_reset_preferences);
  jak2::make_function_symbol_from_c("pc-multi-get-preference-player-limit",
                                    (void*)pc_multi_get_preference_player_limit);
  jak2::make_function_symbol_from_c("pc-multi-set-preference-player-limit",
                                    (void*)pc_multi_set_preference_player_limit);
  jak2::make_function_symbol_from_c("pc-multi-get-preference-player-character",
                                    (void*)pc_multi_get_preference_player_character);
  jak2::make_function_symbol_from_c("pc-multi-set-preference-player-character",
                                    (void*)pc_multi_set_preference_player_character);
  jak2::make_function_symbol_from_c("pc-multi-is-lobby-host", (void*)pc_multi_is_lobby_host);
  jak2::make_function_symbol_from_c("pc-multi-get-session-player-limit",
                                    (void*)pc_multi_get_session_player_limit);
  jak2::make_function_symbol_from_c("pc-multi-lobby-start-game",
                                    (void*)pc_multi_lobby_start_game);
  jak2::make_function_symbol_from_c("pc-multi-lobby-set-character",
                                    (void*)pc_multi_lobby_set_character);
  jak2::make_function_symbol_from_c("pc-multi-get-player-color",
                                    (void*)pc_multi_get_player_color);
  jak2::make_function_symbol_from_c("pc-multi-get-player-appearance",
                                    (void*)pc_multi_get_player_appearance);
  jak2::make_function_symbol_from_c("pc-multi-lobby-set-appearance",
                                    (void*)pc_multi_lobby_set_appearance);
  jak2::make_function_symbol_from_c("pc-multi-get-host-setup-status",
                                    (void*)pc_multi_get_host_setup_status);
  jak2::make_function_symbol_from_c("pc-multi-get-host-port", (void*)pc_multi_get_host_port);
  jak2::make_function_symbol_from_c("pc-multi-get-connection-phase",
                                    (void*)pc_multi_get_connection_phase);
  jak2::make_function_symbol_from_c("pc-multi-get-connection-failure",
                                    (void*)pc_multi_get_connection_failure);
  jak2::make_function_symbol_from_c("pc-multi-poll", (void*)pc_multi_poll);
  jak2::make_function_symbol_from_c("pc-multi-flush-packet-window",
                                    (void*)pc_multi_flush_packet_window);
  jak2::make_function_symbol_from_c("pc-multi-send-sync", (void*)pc_multi_send_sync);
  jak2::make_function_symbol_from_c("pc-multi-receive-sync", (void*)pc_multi_receive_sync);
  jak2::make_function_symbol_from_c("pc-multi-send-events", (void*)pc_multi_send_events);
  jak2::make_function_symbol_from_c("pc-multi-receive-events", (void*)pc_multi_receive_events);
  jak2::make_function_symbol_from_c("pc-multi-send-enemies", (void*)pc_multi_send_enemies);
  jak2::make_function_symbol_from_c("pc-multi-receive-enemies", (void*)pc_multi_receive_enemies);
  jak2::make_function_symbol_from_c("pc-multi-send-traffic", (void*)pc_multi_send_traffic);
  jak2::make_function_symbol_from_c("pc-multi-receive-traffic", (void*)pc_multi_receive_traffic);
  jak2::make_function_symbol_from_c("pc-multi-clear-remote-traffic",
                                    (void*)pc_multi_clear_remote_traffic);
  jak2::make_function_symbol_from_c("pc-multi-publish-traffic-authority-map",
                                    (void*)pc_multi_publish_traffic_authority_map);
  jak2::make_function_symbol_from_c("pc-multi-read-traffic-authority-map",
                                    (void*)pc_multi_read_traffic_authority_map);
  jak2::make_function_symbol_from_c("pc-multi-set-selected-traffic-authority",
                                    (void*)pc_multi_set_selected_traffic_authority);
  jak2::make_function_symbol_from_c("pc-multi-send-palace-squid",
                                    (void*)pc_multi_send_palace_squid);
  jak2::make_function_symbol_from_c("pc-multi-receive-palace-squid",
                                    (void*)pc_multi_receive_palace_squid);
  jak2::make_function_symbol_from_c("pc-multi-send-widow", (void*)pc_multi_send_widow);
  jak2::make_function_symbol_from_c("pc-multi-receive-widow", (void*)pc_multi_receive_widow);
  jak2::make_function_symbol_from_c("pc-multi-send-airlock-state",
                                    (void*)pc_multi_send_airlock_state);
  jak2::make_function_symbol_from_c("pc-multi-receive-airlock-state",
                                    (void*)pc_multi_receive_airlock_state);
  jak2::make_function_symbol_from_c("pc-multi-get-enemy-sync-time",
                                    (void*)pc_multi_get_enemy_sync_time);
  jak2::make_function_symbol_from_c("pc-multi-get-vehicle-sync-time",
                                    (void*)pc_multi_get_vehicle_sync_time);
  jak2::make_function_symbol_from_c("pc-multi-get-role", (void*)pc_multi_get_role);
  jak2::make_function_symbol_from_c("pc-multi-get-local-player-id",
                                    (void*)pc_multi_get_local_player_id);
  jak2::make_function_symbol_from_c("pc-multi-get-host-player-id",
                                    (void*)pc_multi_get_host_player_id);
  jak2::make_function_symbol_from_c("pc-multi-get-local-player-character",
                                    (void*)pc_multi_get_local_player_character);
  jak2::make_function_symbol_from_c("pc-multi-disconnect", (void*)pc_multi_disconnect);
  jak2::make_function_symbol_from_c("pc-multi-reconnect", (void*)pc_multi_reconnect);
  jak2::make_function_symbol_from_c("pc-multi-get-command-line-arg",
                                    (void*)pc_multi_get_command_line_arg);
  jak2::make_function_symbol_from_c("pc-multi-debug-stop-receive",
                                    (void*)pc_multi_debug_stop_receive);
  jak2::make_function_symbol_from_c("pc-multi-get-ticks", (void*)pc_multi_get_ticks);
  jak2::make_function_symbol_from_c("pc-multi-get-ping", (void*)pc_multi_get_ping);
  jak2::make_function_symbol_from_c("pc-multi-get-player-ping", (void*)pc_multi_get_player_ping);
  jak2::make_function_symbol_from_c("pc-multi-get-packet-loss", (void*)pc_multi_get_packet_loss);
  jak2::make_function_symbol_from_c("pc-multi-get-ping-valid", (void*)pc_multi_get_ping_valid);
  jak2::make_function_symbol_from_c("pc-multi-get-packet-loss-percent",
                                    (void*)pc_multi_get_packet_loss_percent);
  jak2::make_function_symbol_from_c("pc-multi-get-packet-loss-variance-percent",
                                    (void*)pc_multi_get_packet_loss_variance_percent);
  jak2::make_function_symbol_from_c("pc-multi-get-ping-variance",
                                    (void*)pc_multi_get_ping_variance);
  jak2::make_function_symbol_from_c("pc-multi-get-total-sent-bytes",
                                    (void*)pc_multi_get_total_sent_bytes);
  jak2::make_function_symbol_from_c("pc-multi-get-total-received-bytes",
                                    (void*)pc_multi_get_total_received_bytes);
  jak2::make_function_symbol_from_c("pc-multi-get-total-sent-packets",
                                    (void*)pc_multi_get_total_sent_packets);
  jak2::make_function_symbol_from_c("pc-multi-get-total-received-packets",
                                    (void*)pc_multi_get_total_received_packets);
  jak2::make_function_symbol_from_c("pc-multi-get-send-rate", (void*)pc_multi_get_send_rate);
  jak2::make_function_symbol_from_c("pc-multi-get-recv-rate", (void*)pc_multi_get_recv_rate);
  jak2::make_function_symbol_from_c("pc-multi-get-send-packet-rate",
                                    (void*)pc_multi_get_send_packet_rate);
  jak2::make_function_symbol_from_c("pc-multi-get-recv-packet-rate",
                                    (void*)pc_multi_get_recv_packet_rate);
  jak2::make_function_symbol_from_c("pc-multi-get-wire-total-sent-bytes",
                                    (void*)pc_multi_get_wire_total_sent_bytes);
  jak2::make_function_symbol_from_c("pc-multi-get-wire-total-received-bytes",
                                    (void*)pc_multi_get_wire_total_received_bytes);
  jak2::make_function_symbol_from_c("pc-multi-get-wire-total-sent-packets",
                                    (void*)pc_multi_get_wire_total_sent_packets);
  jak2::make_function_symbol_from_c("pc-multi-get-wire-total-received-packets",
                                    (void*)pc_multi_get_wire_total_received_packets);
  jak2::make_function_symbol_from_c("pc-multi-get-type-send-rate",
                                    (void*)pc_multi_get_type_send_rate);
  jak2::make_function_symbol_from_c("pc-multi-get-type-recv-rate",
                                    (void*)pc_multi_get_type_recv_rate);
  jak2::make_function_symbol_from_c("pc-multi-get-type-total-sent",
                                    (void*)pc_multi_get_type_total_sent);
  jak2::make_function_symbol_from_c("pc-multi-get-type-total-recv",
                                    (void*)pc_multi_get_type_total_recv);
  jak2::make_function_symbol_from_c("pc-multi-get-type-send-packet-rate",
                                    (void*)pc_multi_get_type_send_packet_rate);
  jak2::make_function_symbol_from_c("pc-multi-get-type-recv-packet-rate",
                                    (void*)pc_multi_get_type_recv_packet_rate);
  jak2::make_function_symbol_from_c("pc-multi-get-type-total-sent-packets",
                                    (void*)pc_multi_get_type_total_sent_packets);
  jak2::make_function_symbol_from_c("pc-multi-get-type-total-received-packets",
                                    (void*)pc_multi_get_type_total_received_packets);
}
