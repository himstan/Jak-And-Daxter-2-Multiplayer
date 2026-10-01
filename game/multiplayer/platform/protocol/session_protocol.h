#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/multiplayer/platform/core/limits.h"
#include "game/multiplayer/platform/core/types.h"

namespace multiplayer::platform {

enum class RejectionReason : uint8_t {
  NONE,
  HOST_FULL,
  WRONG_ROOM_CODE,
  WRONG_GAME,
  VERSION_MISMATCH,
  MALFORMED_GATE,
  GATE_TIMEOUT,
  THROTTLED,
};

struct ClientGate {
  std::string game_id;
  std::string compatibility_identity;
  std::string room_code;
};

struct ServerGate {
  bool accepted = false;
  RejectionReason rejection = RejectionReason::NONE;
  PlayerId participant = kInvalidPlayerId;
  PlayerId host_participant = 0;
  uint8_t participant_capacity = 0;
  PlayerCharacter character = PlayerCharacter::UNKNOWN;
  std::string required_identity;
};

struct ParticipantProfile {
  PlayerId participant = kInvalidPlayerId;
  std::string display_name;
  PlayerCharacter character = PlayerCharacter::UNKNOWN;
  bool ready = false;
  std::vector<uint8_t> game_extension;

  bool operator==(const ParticipantProfile&) const = default;
};

enum class ControlKind : uint8_t {
  PROFILE = 1,
  ROSTER = 2,
  DEPARTURE = 3,
  SET_CHARACTER = 4,
  SET_READY = 5,
  START_COUNTDOWN = 6,
  CANCEL_COUNTDOWN = 7,
  START_GAME = 8,
  BOOTSTRAP_ACK = 9,
  SESSION_CLOSE = 10,
};

bool control_allowed_from(ControlKind kind, SessionRole sender);

struct ControlMessage {
  ControlKind kind = ControlKind::PROFILE;
  ParticipantProfile profile;
  std::vector<ParticipantProfile> roster;
  PlayerId participant = kInvalidPlayerId;
  PlayerCharacter character = PlayerCharacter::UNKNOWN;
  bool ready = false;
  uint32_t value = 0;
  uint8_t reason = 0;
};

struct GameplayEnvelope {
  uint8_t message_id = 0;
  uint32_t sequence = 0;
  std::span<const uint8_t> payload;
};

struct BootstrapEnvelope {
  uint32_t generation = 0;
  std::span<const uint8_t> payload;
};

std::vector<uint8_t> encode_control_message(const ControlMessage& message,
                                            uint16_t maximum_extension_bytes);
bool decode_control_message(std::span<const uint8_t> bytes,
                            uint16_t maximum_extension_bytes,
                            uint8_t maximum_participants,
                            ControlMessage& message);
std::vector<uint8_t> encode_gameplay_envelope(uint8_t message_id,
                                              uint32_t sequence,
                                              std::span<const uint8_t> payload);
bool decode_gameplay_envelope(std::span<const uint8_t> bytes, GameplayEnvelope& envelope);
std::vector<uint8_t> encode_bootstrap_envelope(uint32_t generation,
                                               std::span<const uint8_t> payload);
bool decode_bootstrap_envelope(std::span<const uint8_t> bytes, BootstrapEnvelope& envelope);
std::vector<uint8_t> encode_client_gate(const ClientGate& gate);
bool decode_client_gate(std::span<const uint8_t> bytes, ClientGate& gate);
std::vector<uint8_t> encode_server_gate(const ServerGate& gate);
bool decode_server_gate(std::span<const uint8_t> bytes, ServerGate& gate);

}  // namespace multiplayer::platform
