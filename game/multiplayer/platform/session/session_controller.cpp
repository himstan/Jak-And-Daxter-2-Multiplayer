#include "game/multiplayer/platform/session/session_controller.h"

#include <algorithm>
#include <ranges>

#include "common/log/log.h"

#include "game/multiplayer/platform/core/sequence.h"
#include "game/multiplayer/platform/discovery/invite.h"
#include "game/multiplayer/platform/profile/profile_store.h"
#include "game/multiplayer/platform/protocol/message_frame.h"
#include "game/multiplayer/platform/session/connection_input.h"
#include "game/multiplayer/platform/session/packet_registry.h"

namespace multiplayer::platform {
namespace {
constexpr uint64_t kBootstrapRetryMs = 500;
constexpr uint64_t kGateTimeoutMs = 5000;
constexpr uint64_t kRejectionCloseDelayMs = 100;
constexpr size_t kMaximumPendingGates = 16;
constexpr size_t kMaximumRejectionThrottles = 128;
constexpr uint64_t kRejectionWindowMs = 60'000;
constexpr uint64_t kRejectionThrottleMs = 30'000;
constexpr uint8_t kRejectionsPerWindow = 5;
constexpr int kInvalidProtocolReason = 4;
constexpr int kGameplaySendFailureReason = 2005;
constexpr int kSessionCloseReasonBase = 1000;
constexpr int kSessionCloseReasonMaximum = kSessionCloseReasonBase + 255;

TransportLane gameplay_lane(const GameMessagePolicy& policy) {
  if (policy.delivery == Delivery::RELIABLE_ORDERED)
    return TransportLane::GAMEPLAY_RELIABLE;
  switch (policy.priority) {
    case MessagePriority::CRITICAL:
      return TransportLane::REALTIME_CRITICAL;
    case MessagePriority::BULK:
      return TransportLane::REALTIME_BULK;
    default:
      return TransportLane::REALTIME_NORMAL;
  }
}

bool gameplay_active(const SessionStatus status) {
  return status == SessionStatus::CONNECTING || status == SessionStatus::LOBBY ||
         status == SessionStatus::GAME_STARTING || status == SessionStatus::IN_GAME;
}

}  // namespace

SessionController::SessionController(GameAdapter& adapter)
    : adapter_(adapter), registry_(adapter.descriptor().maximum_players) {
  profiles_.resize(adapter.descriptor().maximum_players);
  adapter_.installed(*this);
}

SessionController::~SessionController() {
  disconnect();
}

bool SessionController::valid_character(const PlayerCharacter character) const {
  return std::ranges::find(adapter_.descriptor().supported_characters, character) !=
         adapter_.descriptor().supported_characters.end();
}

bool SessionController::valid_message_policies(const std::string_view action) const {
  const auto& descriptor = adapter_.descriptor();
  if (!validate_message_policies(adapter_.packets().policies(), descriptor.maximum_payload_bytes)) {
    lg::error("[MP-Session] {} rejected: the game message policy table is invalid.", action);
    return false;
  }
  return true;
}

bool SessionController::valid_game_identity(const std::string_view action) const {
  const auto& descriptor = adapter_.descriptor();
  if (descriptor.game_id.empty()) {
    lg::error("[MP-Session] {} rejected: game ID is empty.", action);
    return false;
  }
  if (descriptor.compatibility_identity.empty()) {
    lg::error("[MP-Session] {} rejected: compatibility identity is unavailable.", action);
    return false;
  }
  return true;
}

void SessionController::reset_adapter_session() {
  if (adapter_session_active_)
    adapter_.session_reset();
  adapter_session_active_ = false;
}

void SessionController::clear_countdown() {
  snapshot_.countdown_active = false;
  snapshot_.countdown_target_ms = 0;
}

uint32_t SessionController::next_bootstrap_generation() {
  if (++host_bootstrap_generation_ == 0)
    ++host_bootstrap_generation_;
  return host_bootstrap_generation_;
}

bool SessionController::validate_profile(PlayerProfile& profile, const PlayerId player_id) const {
  if (player_id >= adapter_.descriptor().maximum_players ||
      !valid_display_name(profile.display_name) || !valid_character(profile.character) ||
      profile.game_extension.size() > adapter_.descriptor().maximum_profile_extension_bytes) {
    return false;
  }
  std::vector<uint8_t> canonical;
  if (!adapter_.validate_profile_extension(profile.game_extension, canonical) ||
      canonical.size() > adapter_.descriptor().maximum_profile_extension_bytes)
    return false;
  profile.player_id = player_id;
  profile.game_extension = std::move(canonical);
  return true;
}

bool SessionController::host(const ControllerHostConfig& config) {
  disconnect();
  const auto& descriptor = adapter_.descriptor();
  if (!valid_message_policies("Host"))
    return false;
  if (config.player_limit < 2 || config.player_limit > descriptor.maximum_players) {
    lg::error("[MP-Session] Host rejected: player limit {} is outside the supported range 2..{}.",
              config.player_limit, descriptor.maximum_players);
    return false;
  }
  if (config.room_code.size() != kMultiplayerRoomCodeLength) {
    lg::error("[MP-Session] Host rejected: room code length is {} instead of {}.",
              config.room_code.size(), kMultiplayerRoomCodeLength);
    return false;
  }
  if (!valid_game_identity("Host"))
    return false;
  if (config.player_characters.size() < config.player_limit ||
      !std::ranges::all_of(std::span(config.player_characters).first(config.player_limit),
                           [this](const auto character) { return valid_character(character); })) {
    lg::error("[MP-Session] Host rejected: player character configuration is invalid.");
    return false;
  }
  local_profile_ = config.local_profile;
  local_profile_.character = config.player_characters[0];
  if (!validate_profile(local_profile_, 0)) {
    lg::error("[MP-Session] Host rejected: local player profile is invalid.");
    return false;
  }
  if (!transport_.host({.port = config.port})) {
    lg::error("[MP-Session] Host transport setup failed on UDP port {}.", config.port);
    return false;
  }
  registry_.configure(config.player_limit);
  player_characters_ = config.player_characters;
  room_code_ = config.room_code;
  snapshot_.state.role = SessionRole::HOST;
  snapshot_.state.status = SessionStatus::CONNECTING;
  snapshot_.state.local_player_id = 0;
  snapshot_.state.host_player_id = 0;
  snapshot_.state.player_limit = config.player_limit;
  profiles_.assign(config.player_limit, {});
  profiles_[0] = local_profile_;
  adapter_.session_started(snapshot_.state);
  adapter_session_active_ = true;
  adapter_.player_profile_changed(local_profile_);
  update_snapshot({});
  lg::debug("[MP-Session] Hosting on UDP port {} for up to {} players.", local_port(),
            config.player_limit);
  return true;
}

bool SessionController::connect(const ControllerClientConfig& config) {
  disconnect();
  if (!valid_message_policies("Connect") || !valid_game_identity("Connect"))
    return false;
  if (config.room_code.size() != kMultiplayerRoomCodeLength) {
    lg::error("[MP-Session] Connect rejected: room code length is {} instead of {}.",
              config.room_code.size(), kMultiplayerRoomCodeLength);
    return false;
  }
  local_profile_ = config.local_profile;
  if (!transport_.connect({.endpoint = config.endpoint, .port = config.port})) {
    lg::error("[MP-Session] Could not start a connection to {}:{}.", config.endpoint, config.port);
    return false;
  }
  room_code_ = config.room_code;
  snapshot_.state.role = SessionRole::CLIENT;
  snapshot_.state.status = SessionStatus::CONNECTING;
  update_snapshot({});
  return true;
}

void SessionController::disconnect(const int reason) {
  const auto close_reason = static_cast<uint8_t>(std::clamp(reason, 0, 255));
  if (snapshot_.state.role == SessionRole::HOST && snapshot_.state.status != SessionStatus::IDLE) {
    send_control({.kind = ControlKind::SESSION_CLOSE, .reason = close_reason},
                 Audience::everyone());
  }
  transport_.shutdown(
      snapshot_.state.role == SessionRole::HOST ? kSessionCloseReasonBase + close_reason : reason,
      snapshot_.state.role == SessionRole::HOST);
  cadence_.reset();
  pressure_.reset();
  registry_.reset();
  outbound_sequences_ = {};
  profiles_.assign(adapter_.descriptor().maximum_players, {});
  local_profile_ = {};
  pending_gates_.clear();
  pending_rejection_closes_.clear();
  rejection_throttles_.clear();
  player_characters_.clear();
  room_code_.clear();
  last_applied_bootstrap_ = 0;
  host_bootstrap_generation_ = 0;
  bootstrap_send_deferred_ = false;
  gameplay_send_observed_ = false;
  gameplay_receive_observed_ = false;
  gameplay_rejection_observed_ = false;
  snapshot_ = {};
  reset_adapter_session();
}

void SessionController::pump(const uint64_t now_ms) {
  last_pump_ms_ = now_ms;
  transport_.pump();
  pressure_.update(classify_network_pressure(transport_.aggregate_snapshot()), now_ms);
  TransportEvent event;
  while (transport_.poll_event(event))
    handle_event(std::move(event), now_ms);
  if (snapshot_.state.role == SessionRole::HOST) {
    std::vector<ConnectionId> expired;
    for (const auto& [connection, pending] : pending_gates_) {
      if (now_ms >= pending.deadline_ms)
        expired.push_back(connection);
    }
    for (const auto connection : expired)
      reject(connection, RejectionReason::GATE_TIMEOUT);
    std::vector<ConnectionId> rejected;
    for (const auto& [connection, deadline] : pending_rejection_closes_) {
      if (now_ms >= deadline)
        rejected.push_back(connection);
    }
    for (const auto connection : rejected) {
      transport_.close_connection(connection, kInvalidProtocolReason, "admission rejected");
      pending_rejection_closes_.erase(connection);
    }
  }
  if (snapshot_.state.role == SessionRole::HOST)
    send_pending_bootstraps(now_ms);
  auto connections = transport_.connection_snapshots();
  snapshot_.statistics = aggregate_connection_snapshots(connections);
  update_snapshot(std::move(connections));
}

void SessionController::handle_event(TransportEvent event, const uint64_t now_ms) {
  if (event.kind == TransportEventKind::CONNECTED)
    handle_connected(event, now_ms);
  else if (event.kind == TransportEventKind::MESSAGE) {
    if (registry_.find_connection(event.connection_id))
      handle_admitted_message(event);
    else
      handle_pending_message(event);
  } else if (snapshot_.state.role == SessionRole::HOST) {
    pending_gates_.erase(event.connection_id);
    pending_rejection_closes_.erase(event.connection_id);
    host_departure(event.connection_id, event.close_reason);
  } else {
    lg::debug("[MP-Session] Host connection closed (reason {}, detail '{}').", event.close_reason,
              event.detail);
    if (event.close_reason >= kSessionCloseReasonBase &&
        event.close_reason <= kSessionCloseReasonMaximum) {
      snapshot_.state.status = SessionStatus::HOST_LEFT;
      snapshot_.close_reason = static_cast<uint8_t>(event.close_reason - kSessionCloseReasonBase);
      clear_countdown();
      reset_adapter_session();
    } else if (snapshot_.state.status != SessionStatus::FAILED &&
               snapshot_.state.status != SessionStatus::HOST_LEFT) {
      snapshot_.state.status = SessionStatus::RECONNECTING;
      last_applied_bootstrap_ = 0;
      reset_adapter_session();
    }
  }
}

void SessionController::handle_connected(const TransportEvent& event, const uint64_t now_ms) {
  if (snapshot_.state.role == SessionRole::HOST) {
    lg::debug("[MP-Session] Incoming transport connection from {} (connection {}).",
              event.remote_address, event.connection_id);
    if (!rejection_throttles_.contains(event.remote_address) &&
        rejection_throttles_.size() >= kMaximumRejectionThrottles) {
      auto oldest = rejection_throttles_.begin();
      for (auto candidate = rejection_throttles_.begin(); candidate != rejection_throttles_.end();
           ++candidate) {
        if (candidate->second.blocked_until_ms < oldest->second.blocked_until_ms ||
            (candidate->second.blocked_until_ms == oldest->second.blocked_until_ms &&
             candidate->second.window_started_ms < oldest->second.window_started_ms)) {
          oldest = candidate;
        }
      }
      rejection_throttles_.erase(oldest);
    }
    const auto& throttle = rejection_throttles_[event.remote_address];
    pending_gates_[event.connection_id] = {.deadline_ms = now_ms + kGateTimeoutMs,
                                           .remote_address = event.remote_address};
    if (throttle.blocked_until_ms > now_ms || pending_gates_.size() > kMaximumPendingGates) {
      reject(event.connection_id, RejectionReason::THROTTLED);
      return;
    }
  } else {
    lg::debug("[MP-Session] Transport connected to {} (connection {}); sending admission gate.",
              event.remote_address, event.connection_id);
    const auto& descriptor = adapter_.descriptor();
    const auto gate =
        encode_client_gate({.game_id = descriptor.game_id,
                            .compatibility_identity = descriptor.compatibility_identity,
                            .room_code = room_code_});
    if (gate.empty() ||
        !transport_.send(event.connection_id, gate, TransportLane::CONTROL_RELIABLE)) {
      transport_.close_connection(event.connection_id, kInvalidProtocolReason,
                                  "could not send admission gate");
    }
  }
}

void SessionController::handle_pending_message(const TransportEvent& event) {
  if (event.delivery != Delivery::RELIABLE_ORDERED) {
    transport_.close_connection(event.connection_id, kInvalidProtocolReason,
                                "admission gate must be reliable");
    return;
  }
  if (snapshot_.state.role == SessionRole::HOST) {
    if (!pending_gates_.contains(event.connection_id))
      return;
    ClientGate gate;
    const auto& descriptor = adapter_.descriptor();
    if (!decode_client_gate(event.payload, gate))
      return reject(event.connection_id, RejectionReason::MALFORMED_GATE);
    if (gate.compatibility_identity != descriptor.compatibility_identity) {
      return reject(event.connection_id, RejectionReason::VERSION_MISMATCH,
                    descriptor.compatibility_identity);
    }
    if (gate.game_id != descriptor.game_id)
      return reject(event.connection_id, RejectionReason::WRONG_GAME);
    if (gate.room_code != room_code_)
      return reject(event.connection_id, RejectionReason::WRONG_ROOM_CODE);
    const PlayerId next = [&]() {
      for (PlayerId id = 1; id < snapshot_.state.player_limit; ++id) {
        if (!registry_.find_player(id))
          return id;
      }
      return kInvalidPlayerId;
    }();
    if (next == kInvalidPlayerId)
      return reject(event.connection_id, RejectionReason::HOST_FULL);
    const auto character = player_characters_[next];
    if (!registry_.bind(event.connection_id, next, character))
      return reject(event.connection_id, RejectionReason::HOST_FULL);
    pending_gates_.erase(event.connection_id);
    lg::debug("[MP-Session] Accepted connection {} as player {}.", event.connection_id, next);
    const auto response = encode_server_gate({.accepted = true,
                                              .player_id = next,
                                              .host_player_id = 0,
                                              .player_capacity = snapshot_.state.player_limit,
                                              .character = character});
    if (!transport_.send(event.connection_id, response, TransportLane::CONTROL_RELIABLE)) {
      host_departure(event.connection_id, kInvalidProtocolReason);
      transport_.close_connection(event.connection_id, kInvalidProtocolReason,
                                  "could not accept admission gate");
    }
    return;
  }
  ServerGate gate;
  if (!decode_server_gate(event.payload, gate)) {
    transport_.close_connection(event.connection_id, kInvalidProtocolReason,
                                "invalid server admission gate");
    return;
  }
  if (!gate.accepted) {
    snapshot_.state.status = SessionStatus::FAILED;
    snapshot_.rejection = gate.rejection;
    snapshot_.required_identity = std::move(gate.required_identity);
    transport_.close_connection(event.connection_id, kInvalidProtocolReason, "admission rejected");
    return;
  }
  if (gate.player_id == gate.host_player_id || gate.host_player_id != 0 ||
      gate.player_id >= gate.player_capacity || gate.player_capacity < 2 ||
      !valid_character(gate.character) || !registry_.configure(gate.player_capacity) ||
      !registry_.bind(event.connection_id, gate.host_player_id, PlayerCharacter::UNKNOWN)) {
    transport_.close_connection(event.connection_id, kInvalidProtocolReason,
                                "invalid player assignment");
    return;
  }
  snapshot_.state.status = SessionStatus::LOBBY;
  snapshot_.state.local_player_id = gate.player_id;
  snapshot_.state.host_player_id = gate.host_player_id;
  snapshot_.state.player_limit = gate.player_capacity;
  profiles_.assign(gate.player_capacity, {});
  local_profile_.player_id = gate.player_id;
  local_profile_.character = gate.character;
  if (!validate_profile(local_profile_, gate.player_id)) {
    transport_.close_connection(event.connection_id, kInvalidProtocolReason,
                                "invalid local profile");
    return;
  }
  adapter_.session_started(snapshot_.state);
  adapter_session_active_ = true;
  lg::debug("[MP-Session] Admission accepted; assigned local player {} (host {}).", gate.player_id,
            gate.host_player_id);
  send_control({.kind = ControlKind::PROFILE, .profile = local_profile_},
               Audience::one(event.connection_id));
}

void SessionController::host_departure(const ConnectionId connection, const int reason) {
  const auto* player = registry_.find_connection(connection);
  if (!player)
    return;
  const auto id = player->player_id;
  if (id >= profiles_.size())
    return;
  profiles_[id] = {};
  registry_.release_connection(connection);
  lg::debug("[MP-Session] Player {} (connection {}) departed with reason {}.", id, connection,
            reason);
  send_control({.kind = ControlKind::DEPARTURE,
                .player_id = id,
                .reason = static_cast<uint8_t>(std::clamp(reason, 0, 255))},
               Audience::everyone());
  adapter_.player_departed(id);
  clear_countdown();
}

void SessionController::handle_admitted_message(const TransportEvent& event) {
  const auto* player = registry_.find_connection(event.connection_id);
  if (!player)
    return;
  MessageFrame frame;
  if (!decode_message_frame(event.payload, frame) ||
      (snapshot_.state.role == SessionRole::HOST && frame.origin != player->player_id) ||
      (snapshot_.state.role == SessionRole::CLIENT &&
       event.connection_id != transport_.host_connection_id())) {
    transport_.close_connection(event.connection_id, kInvalidProtocolReason,
                                "invalid authenticated origin");
    return;
  }
  if (frame.kind == FrameKind::CONTROL) {
    handle_control(event.connection_id, player->player_id, frame.origin, event.delivery,
                   frame.payload, last_pump_ms_);
  } else if (frame.kind == FrameKind::BOOTSTRAP) {
    handle_bootstrap(event.connection_id, frame.origin, event.delivery, frame.payload);
  } else {
    handle_gameplay(event.connection_id, player->player_id, frame.origin, event.delivery,
                    event.lane, frame.payload);
  }
}

void SessionController::handle_control(const ConnectionId connection,
                                       const PlayerId player_id,
                                       const PlayerId origin_id,
                                       const Delivery delivery,
                                       const std::span<const uint8_t> payload,
                                       const uint64_t now_ms) {
  ControlMessage message;
  if (delivery != Delivery::RELIABLE_ORDERED ||
      !decode_control_message(payload, adapter_.descriptor().maximum_profile_extension_bytes,
                              snapshot_.state.player_limit, message)) {
    transport_.close_connection(connection, kInvalidProtocolReason, "invalid control message");
    return;
  }
  if (snapshot_.state.role == SessionRole::HOST)
    handle_host_control(connection, player_id, std::move(message));
  else if (connection == transport_.host_connection_id() &&
           origin_id == snapshot_.state.host_player_id)
    handle_client_control(connection, std::move(message), now_ms);
}

void SessionController::handle_host_control(const ConnectionId connection,
                                            const PlayerId player_id,
                                            ControlMessage message) {
  if (!control_allowed_from(message.kind, SessionRole::CLIENT)) {
    transport_.close_connection(connection, kInvalidProtocolReason, "unauthorized control message");
    return;
  }
  if (player_id == 0 || player_id >= profiles_.size())
    return;
  auto& profile = profiles_[player_id];
  if (message.kind == ControlKind::PROFILE) {
    if (!validate_profile(message.profile, player_id)) {
      transport_.close_connection(connection, kInvalidProtocolReason, "invalid player profile");
      return;
    }
    message.profile.character = player_characters_[player_id];
    profile = std::move(message.profile);
    if (auto* session = registry_.find_player(player_id)) {
      session->profile = profile;
      session->identity_ready = true;
    }
    lg::debug("[MP-Session] Player {} profile accepted; gameplay and bootstrap are ready.",
              player_id);
    publish_roster(connection);
    broadcast_profile(profile);
    adapter_.player_profile_changed(profile);
    if (snapshot_.countdown_active && snapshot_.countdown_target_ms > last_pump_ms_) {
      const auto remaining_ms = snapshot_.countdown_target_ms - last_pump_ms_;
      send_control({.kind = ControlKind::START_COUNTDOWN,
                    .value = static_cast<uint32_t>((remaining_ms + 999) / 1000)},
                   Audience::one(connection));
    }
    if (snapshot_.state.status == SessionStatus::IN_GAME) {
      if (auto* session = registry_.find_player(player_id)) {
        session->bootstrap_pending = true;
        session->bootstrap_sent_once = false;
        session->bootstrap_payload.clear();
        session->bootstrap_generation = next_bootstrap_generation();
      }
    }
  } else if (message.kind == ControlKind::SET_CHARACTER) {
    if (!valid_character(message.character)) {
      transport_.close_connection(connection, kInvalidProtocolReason, "invalid player character");
      return;
    }
    profile.character = message.character;
    if (auto* session = registry_.find_player(player_id)) {
      session->character = message.character;
      session->profile = profile;
    }
    broadcast_profile(profile);
    adapter_.player_profile_changed(profile);
  } else if (message.kind == ControlKind::SET_READY) {
    profile.ready = message.ready;
    if (auto* session = registry_.find_player(player_id))
      session->profile = profile;
    broadcast_profile(profile);
    adapter_.player_profile_changed(profile);
  } else if (message.kind == ControlKind::BOOTSTRAP_ACK) {
    registry_.acknowledge_bootstrap(player_id, message.value);
  }
}

void SessionController::handle_client_control(const ConnectionId connection,
                                              ControlMessage message,
                                              const uint64_t now_ms) {
  if (!control_allowed_from(message.kind, SessionRole::HOST)) {
    transport_.close_connection(connection, kInvalidProtocolReason, "unauthorized control message");
    return;
  }
  if (message.kind == ControlKind::PROFILE) {
    if (message.profile.player_id >= profiles_.size() ||
        !validate_profile(message.profile, message.profile.player_id)) {
      transport_.close_connection(connection, kInvalidProtocolReason, "invalid player profile");
      return;
    }
    const bool changed = profiles_[message.profile.player_id] != message.profile;
    profiles_[message.profile.player_id] = message.profile;
    if (message.profile.player_id == snapshot_.state.local_player_id) {
      local_profile_ = message.profile;
    }
    if (changed)
      adapter_.player_profile_changed(message.profile);
  } else if (message.kind == ControlKind::ROSTER) {
    std::vector<PlayerProfile> roster(profiles_.size());
    std::vector<bool> seen(profiles_.size(), false);
    for (auto profile : message.roster) {
      if (profile.player_id >= roster.size() || seen[profile.player_id] ||
          !validate_profile(profile, profile.player_id)) {
        transport_.close_connection(connection, kInvalidProtocolReason, "invalid player roster");
        return;
      }
      seen[profile.player_id] = true;
      roster[profile.player_id] = std::move(profile);
    }
    const auto previous = std::move(profiles_);
    profiles_ = std::move(roster);
    for (const auto& profile : profiles_) {
      if (profile.player_id == kInvalidPlayerId)
        continue;
      if (profile.player_id == snapshot_.state.local_player_id)
        local_profile_ = profile;
      if (profile.player_id >= previous.size() || previous[profile.player_id] != profile) {
        adapter_.player_profile_changed(profile);
      }
    }
  } else if (message.kind == ControlKind::DEPARTURE) {
    if (message.player_id == snapshot_.state.host_player_id ||
        message.player_id >= profiles_.size()) {
      transport_.close_connection(connection, kInvalidProtocolReason, "invalid player departure");
      return;
    }
    profiles_[message.player_id] = {};
    adapter_.player_departed(message.player_id);
    clear_countdown();
  } else if (message.kind == ControlKind::START_COUNTDOWN) {
    snapshot_.countdown_active = true;
    snapshot_.countdown_target_ms = now_ms + static_cast<uint64_t>(message.value) * 1000;
  } else if (message.kind == ControlKind::CANCEL_COUNTDOWN) {
    clear_countdown();
  } else if (message.kind == ControlKind::START_GAME) {
    snapshot_.state.status = SessionStatus::GAME_STARTING;
    clear_countdown();
  } else if (message.kind == ControlKind::SESSION_CLOSE) {
    snapshot_.state.status = SessionStatus::HOST_LEFT;
    snapshot_.close_reason = message.reason;
    clear_countdown();
    reset_adapter_session();
    transport_.close_connection(connection, message.reason, "session closed by host");
  }
}

void SessionController::handle_gameplay(const ConnectionId connection,
                                        const PlayerId player_id,
                                        const PlayerId origin,
                                        const Delivery delivery,
                                        const TransportLane lane,
                                        const std::span<const uint8_t> payload) {
  GameplayEnvelope envelope;
  if (!decode_gameplay_envelope(payload, envelope) || origin >= snapshot_.state.player_limit ||
      (snapshot_.state.role == SessionRole::CLIENT && origin == snapshot_.state.local_player_id)) {
    transport_.close_connection(connection, kInvalidProtocolReason, "invalid gameplay envelope");
    return;
  }
  const auto* policy = find_message_policy(adapter_.packets().policies(), envelope.message_id);
  if (!policy || envelope.payload.size() > policy->maximum_payload_bytes ||
      !can_receive_message(*policy, snapshot_.state.role) || policy->delivery != delivery ||
      gameplay_lane(*policy) != lane) {
    transport_.close_connection(connection, kInvalidProtocolReason, "gameplay policy violation");
    return;
  }
  const GameplayMessage message = {
      .origin = {.connection_id = connection,
                 .authenticated_player_id = origin,
                 .from_host = snapshot_.state.role == SessionRole::CLIENT},
      .message_id = envelope.message_id,
      .sequence = envelope.sequence,
      .received_at_ms = last_pump_ms_,
      .payload = envelope.payload};
  auto [disposition, canonical_payload, relay_recipients] = adapter_.packets().receive(message, *this);
  if (disposition == PayloadDisposition::REJECT) {
    if (!gameplay_rejection_observed_) {
      gameplay_rejection_observed_ = true;
      lg::warn("[MP-Session] Game adapter rejected the first received '{}' payload from player {}.",
               policy->name, origin);
    }
    return;
  }
  if (!gameplay_receive_observed_) {
    gameplay_receive_observed_ = true;
    lg::debug("[MP-Session] Gameplay receive path active: accepted '{}' from player {}.",
              policy->name, origin);
  }
  if (disposition == PayloadDisposition::CONSUME_AND_RELAY &&
      snapshot_.state.role == SessionRole::HOST &&
      canonical_payload.size() <= policy->maximum_payload_bytes) {
    const auto bytes =
        encode_gameplay_envelope(envelope.message_id, envelope.sequence, canonical_payload);
    if (!relay_recipients) {
      send_frame(FrameKind::GAMEPLAY, Audience::everyone_except_origin(), origin, bytes,
                 gameplay_lane(*policy));
    } else {
      std::array<bool, static_cast<size_t>(kInvalidPlayerId) + 1> relayed = {};
      for (const auto target : *relay_recipients) {
        if (target >= snapshot_.state.player_limit || target == origin || relayed[target]) {
          lg::warn("[MP-Session] Adapter supplied an invalid relay target {} for '{}'.", target,
                   policy->name);
          continue;
        }
        const auto* player_session = registry_.find_player(target);
        if (!player_session || !player_session->accepted)
          continue;
        relayed[target] = true;
        send_frame(FrameKind::GAMEPLAY, Audience::one(player_session->connection_id), origin, bytes,
                   gameplay_lane(*policy));
      }
    }
  }
}

void SessionController::handle_bootstrap(const ConnectionId connection,
                                         const PlayerId origin,
                                         const Delivery delivery,
                                         const std::span<const uint8_t> payload) {
  if (snapshot_.state.role != SessionRole::CLIENT ||
      connection != transport_.host_connection_id() || origin != snapshot_.state.host_player_id ||
      delivery != Delivery::RELIABLE_ORDERED) {
    transport_.close_connection(connection, kInvalidProtocolReason, "unauthorized bootstrap");
    return;
  }
  BootstrapEnvelope envelope;
  if (!decode_bootstrap_envelope(payload, envelope) || envelope.generation == 0) {
    transport_.close_connection(connection, kInvalidProtocolReason, "invalid bootstrap envelope");
    return;
  }
  if (last_applied_bootstrap_ != 0 &&
      !sequence_is_newer(envelope.generation, last_applied_bootstrap_)) {
    if (envelope.generation != last_applied_bootstrap_)
      return;
  } else if (!adapter_.apply_bootstrap(envelope.generation, envelope.payload)) {
    return;
  }
  {
    last_applied_bootstrap_ = envelope.generation;
    send_control({.kind = ControlKind::BOOTSTRAP_ACK, .value = envelope.generation},
                 Audience::one(connection));
  }
}

std::optional<uint32_t> SessionController::send_gameplay(const uint8_t message_id,
                                                         const Audience& audience,
                                                         const std::span<const uint8_t> payload) {
  const auto* policy = find_message_policy(adapter_.packets().policies(), message_id);
  if (!gameplay_active(snapshot_.state.status) || !policy ||
      !can_send_message(*policy, snapshot_.state.role) ||
      payload.size() > policy->maximum_payload_bytes ||
      snapshot_.state.local_player_id == kInvalidPlayerId)
    return std::nullopt;
  auto& sequence = outbound_sequences_[message_id];
  advance_nonzero_sequence(sequence);
  const auto bytes = encode_gameplay_envelope(message_id, sequence, payload);
  const auto result = send_frame(FrameKind::GAMEPLAY, audience, snapshot_.state.local_player_id,
                                 bytes, gameplay_lane(*policy));
  if (result == FrameSendResult::REJECTED)
    return std::nullopt;
  if (result == FrameSendResult::QUEUED && !gameplay_send_observed_) {
    gameplay_send_observed_ = true;
    lg::debug("[MP-Session] Gameplay send path active: sent '{}'.", policy->name);
  }
  return sequence;
}

bool SessionController::cadence_due(const uint8_t message_id,
                                    const uint64_t now_ms,
                                    const bool dirty) {
  const auto* policy = find_message_policy(adapter_.packets().policies(), message_id);
  return policy && cadence_.due(*policy, now_ms, dirty, pressure_.pressure());
}

uint32_t SessionController::estimated_rtt_ms(const PlayerId player_id) const {
  const auto connections = transport_.connection_snapshots();
  if (snapshot_.state.role == SessionRole::CLIENT && !connections.empty()) {
    return static_cast<uint32_t>((std::max)(connections.front().ping_ms, 0));
  }
  for (const auto& connection : connections) {
    if (const auto* binding = registry_.find_connection(connection.connection_id);
        binding && binding->player_id == player_id) {
      return static_cast<uint32_t>((std::max)(connection.ping_ms, 0));
    }
  }
  return 0;
}

bool SessionController::severe_pressure_sustained(const uint64_t now_ms) const {
  return pressure_.pressure() == NetworkPressure::SEVERE &&
         now_ms - pressure_.severe_since_ms() >= 2000;
}

std::string SessionController::invite_for_address(const std::string_view address) const {
  if (snapshot_.state.role != SessionRole::HOST || address.empty())
    return {};
  return make_invite(address, transport_.local_port(), room_code_);
}

void SessionController::reject(const ConnectionId connection,
                               const RejectionReason reason,
                               const std::string_view required) {
  const auto pending = pending_gates_.find(connection);
  if (pending == pending_gates_.end())
    return;
  lg::warn("[MP-Session] Rejected pending connection {} from {} (reason {}).", connection,
           pending->second.remote_address, static_cast<uint8_t>(reason));
  auto& [window_started_ms, blocked_until_ms, count] =
      rejection_throttles_[pending->second.remote_address];
  if (last_pump_ms_ - window_started_ms >= kRejectionWindowMs) {
    window_started_ms = last_pump_ms_;
    count = 0;
  }
  if (++count >= kRejectionsPerWindow) {
    blocked_until_ms = last_pump_ms_ + kRejectionThrottleMs;
  }
  const auto response =
      encode_server_gate({.rejection = reason, .required_identity = std::string(required)});
  transport_.send(connection, response, TransportLane::CONTROL_RELIABLE);
  pending_gates_.erase(pending);
  pending_rejection_closes_[connection] = last_pump_ms_ + kRejectionCloseDelayMs;
}

FrameSendResult SessionController::send_frame(const FrameKind kind,
                                              const Audience& audience,
                                              const PlayerId origin,
                                              const std::span<const uint8_t> payload,
                                              const TransportLane lane) {
  if (payload.empty() || origin == kInvalidPlayerId)
    return FrameSendResult::REJECTED;
  if (snapshot_.state.role == SessionRole::HOST) {
    if (origin != snapshot_.state.local_player_id && !registry_.find_player(origin))
      return FrameSendResult::REJECTED;
  } else if (snapshot_.state.role == SessionRole::CLIENT) {
    if (origin != snapshot_.state.local_player_id)
      return FrameSendResult::REJECTED;
  } else {
    return FrameSendResult::REJECTED;
  }
  const auto frame = encode_message_frame(kind, origin, payload);
  if (frame.empty())
    return FrameSendResult::REJECTED;
  std::vector<ConnectionId> recipients;
  if (audience.kind == AudienceKind::CONNECTION) {
    recipients.push_back(audience.connection_id);
  } else if (snapshot_.state.role == SessionRole::CLIENT) {
    recipients.push_back(transport_.host_connection_id());
  } else {
    for (const auto& player : registry_.entries()) {
      if (!player.accepted)
        continue;
      if (audience.kind == AudienceKind::EVERYONE_EXCEPT_ORIGIN && player.player_id == origin)
        continue;
      recipients.push_back(player.connection_id);
    }
  }
  return submit_frame(
      recipients, frame, kind, lane,
      [this](const auto connection, const auto bytes, const auto selected_lane) {
        return transport_.send(connection, bytes, selected_lane);
      },
      [this](const auto connection) {
        transport_.close_connection(connection, kGameplaySendFailureReason,
                                    "reliable gameplay submission failed");
      });
}

void SessionController::request_bootstrap() {
  if (snapshot_.state.role != SessionRole::HOST)
    return;
  registry_.request_bootstrap_for_all(next_bootstrap_generation());
  bootstrap_send_deferred_ = true;
}

void SessionController::send_pending_bootstraps(const uint64_t now_ms) {
  if (bootstrap_send_deferred_) {
    bootstrap_send_deferred_ = false;
    return;
  }
  for (auto& player : registry_.entries()) {
    if (!player.bootstrap_pending || !player.identity_ready ||
        (player.bootstrap_sent_once &&
         now_ms - player.last_bootstrap_send_time < kBootstrapRetryMs))
      continue;
    if (player.bootstrap_payload.empty()) {
      player.bootstrap_payload = adapter_.create_bootstrap(player.player_id);
      if (player.bootstrap_payload.empty() ||
          player.bootstrap_payload.size() > adapter_.descriptor().maximum_payload_bytes) {
        continue;
      }
    }
    const auto bytes =
        encode_bootstrap_envelope(player.bootstrap_generation, player.bootstrap_payload);
    if (send_frame(FrameKind::BOOTSTRAP, Audience::one(player.connection_id), 0, bytes,
                   TransportLane::CONTROL_RELIABLE) == FrameSendResult::QUEUED) {
      player.bootstrap_sent_once = true;
      player.last_bootstrap_send_time = now_ms;
    }
  }
}

void SessionController::send_control(const ControlMessage& message, const Audience& audience) {
  const auto bytes =
      encode_control_message(message, adapter_.descriptor().maximum_profile_extension_bytes);
  if (!bytes.empty() && snapshot_.state.local_player_id != kInvalidPlayerId) {
    send_frame(FrameKind::CONTROL, audience, snapshot_.state.local_player_id, bytes,
               TransportLane::CONTROL_RELIABLE);
  }
}

void SessionController::broadcast_profile(const PlayerProfile& profile) {
  send_control({.kind = ControlKind::PROFILE, .profile = profile}, Audience::everyone());
}

void SessionController::publish_roster(const ConnectionId connection) {
  ControlMessage roster = {.kind = ControlKind::ROSTER};
  for (const auto& profile : profiles_) {
    if (profile.player_id != kInvalidPlayerId)
      roster.roster.push_back(profile);
  }
  send_control(roster, Audience::one(connection));
}

bool SessionController::set_local_profile(PlayerProfile profile) {
  if (snapshot_.state.role == SessionRole::NONE ||
      (snapshot_.state.status != SessionStatus::LOBBY &&
       snapshot_.state.status != SessionStatus::IN_GAME))
    return false;
  if (!validate_profile(profile, snapshot_.state.local_player_id))
    return false;
  local_profile_ = profile;
  if (snapshot_.state.role == SessionRole::HOST) {
    profiles_[profile.player_id] = profile;
    broadcast_profile(profile);
    adapter_.player_profile_changed(profile);
  } else {
    send_control({.kind = ControlKind::PROFILE, .profile = profile},
                 Audience::one(transport_.host_connection_id()));
  }
  return true;
}

bool SessionController::set_character(const PlayerCharacter character) {
  if (snapshot_.state.status != SessionStatus::LOBBY || !valid_character(character))
    return false;
  local_profile_.character = character;
  if (snapshot_.state.role == SessionRole::HOST)
    return set_local_profile(local_profile_);
  send_control({.kind = ControlKind::SET_CHARACTER, .character = character},
               Audience::one(transport_.host_connection_id()));
  return true;
}

bool SessionController::set_ready(const bool ready) {
  if (snapshot_.state.status != SessionStatus::LOBBY)
    return false;
  local_profile_.ready = ready;
  if (snapshot_.state.role == SessionRole::HOST)
    return set_local_profile(local_profile_);
  send_control({.kind = ControlKind::SET_READY, .ready = ready},
               Audience::one(transport_.host_connection_id()));
  return true;
}

bool SessionController::start_countdown(const uint32_t seconds) {
  if (snapshot_.state.role != SessionRole::HOST || snapshot_.state.status != SessionStatus::LOBBY ||
      seconds == 0)
    return false;
  snapshot_.countdown_active = true;
  snapshot_.countdown_target_ms = last_pump_ms_ + static_cast<uint64_t>(seconds) * 1000;
  send_control({.kind = ControlKind::START_COUNTDOWN, .value = seconds}, Audience::everyone());
  return true;
}

bool SessionController::cancel_countdown() {
  if (snapshot_.state.role != SessionRole::HOST || snapshot_.state.status != SessionStatus::LOBBY ||
      !snapshot_.countdown_active)
    return false;
  clear_countdown();
  send_control({.kind = ControlKind::CANCEL_COUNTDOWN}, Audience::everyone());
  return true;
}

bool SessionController::start_game() {
  if (snapshot_.state.role != SessionRole::HOST || snapshot_.state.status != SessionStatus::LOBBY)
    return false;
  snapshot_.state.status = SessionStatus::GAME_STARTING;
  snapshot_.countdown_active = false;
  send_control({.kind = ControlKind::START_GAME}, Audience::everyone());
  return true;
}

void SessionController::enter_game() {
  if (snapshot_.state.role != SessionRole::NONE) {
    const bool entering_game = snapshot_.state.status != SessionStatus::IN_GAME;
    snapshot_.state.status = SessionStatus::IN_GAME;
    if (snapshot_.state.role == SessionRole::HOST && entering_game)
      request_bootstrap();
  }
}

void SessionController::enter_lobby() {
  if (snapshot_.state.role != SessionRole::NONE)
    snapshot_.state.status = SessionStatus::LOBBY;
}

void SessionController::update_snapshot(std::vector<ConnectionSnapshot> connections) {
  snapshot_.players.clear();
  for (const auto& profile : profiles_) {
    if (profile.player_id != kInvalidPlayerId)
      snapshot_.players.push_back(profile);
  }
  snapshot_.connections.clear();
  snapshot_.connections.reserve(connections.size());
  for (auto& connection : connections) {
    const auto* binding = registry_.find_connection(connection.connection_id);
    snapshot_.connections.push_back({.player_id = binding ? binding->player_id : kInvalidPlayerId,
                                     .network = std::move(connection)});
  }
  std::ranges::sort(snapshot_.connections, [](const SessionConnectionSnapshot& left,
                                              const SessionConnectionSnapshot& right) {
    return left.player_id < right.player_id;
  });
}

}  // namespace multiplayer::platform
