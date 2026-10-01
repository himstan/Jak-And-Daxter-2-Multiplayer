#include "game/multiplayer/platform/protocol/session_protocol.h"

#include <algorithm>
#include <array>
#include <limits>

namespace multiplayer::platform {
namespace {
constexpr std::array<uint8_t, 4> kGateMagic = {'J', 'M', 'P', 'G'};
constexpr uint8_t kClientGateKind = 1;
constexpr uint8_t kAcceptedGateKind = 2;
constexpr uint8_t kRejectedGateKind = 3;
void write_u16(std::vector<uint8_t>& out, const uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}
void write_u32(std::vector<uint8_t>& out, const uint32_t value) {
  for (uint32_t shift = 0; shift < 32; shift += 8)
    out.push_back(value >> shift);
}
bool read_u8(std::span<const uint8_t> bytes, size_t& cursor, uint8_t& value) {
  if (cursor == bytes.size())
    return false;
  value = bytes[cursor++];
  return true;
}
bool read_u16(std::span<const uint8_t> bytes, size_t& cursor, uint16_t& value) {
  if (bytes.size() - cursor < 2)
    return false;
  value = static_cast<uint16_t>(bytes[cursor]) |
          static_cast<uint16_t>(static_cast<uint16_t>(bytes[cursor + 1]) << 8);
  cursor += 2;
  return true;
}
bool read_u32(std::span<const uint8_t> bytes, size_t& cursor, uint32_t& value) {
  if (bytes.size() - cursor < 4)
    return false;
  value = 0;
  for (uint32_t shift = 0; shift < 32; shift += 8) {
    value |= static_cast<uint32_t>(bytes[cursor++]) << shift;
  }
  return true;
}
bool write_string(std::vector<uint8_t>& out, const std::string_view value) {
  if (value.size() > (std::numeric_limits<uint16_t>::max)())
    return false;
  write_u16(out, static_cast<uint16_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
  return true;
}
bool read_string(const std::span<const uint8_t> bytes, size_t& cursor, std::string& value) {
  uint16_t size = 0;
  if (!read_u16(bytes, cursor, size) || bytes.size() - cursor < size)
    return false;
  value.assign(reinterpret_cast<const char*>(bytes.data() + cursor), size);
  cursor += size;
  return true;
}
bool read_gate_header(const std::span<const uint8_t> bytes,
                      const uint8_t expected_kind,
                      size_t& cursor) {
  if (bytes.size() < kGateMagic.size() + 1 ||
      !std::equal(kGateMagic.begin(), kGateMagic.end(), bytes.begin()) ||
      bytes[kGateMagic.size()] != expected_kind)
    return false;
  cursor = kGateMagic.size() + 1;
  return true;
}
bool valid_character(const PlayerCharacter character) {
  return character == PlayerCharacter::JAK || character == PlayerCharacter::DAXTER;
}
bool encode_profile(std::vector<uint8_t>& out,
                    const ParticipantProfile& profile,
                    const uint16_t maximum_extension_bytes) {
  if (profile.participant == kInvalidPlayerId ||
      profile.display_name.size() > kMaximumDisplayNameBytes ||
      profile.game_extension.size() > maximum_extension_bytes ||
      profile.game_extension.size() > (std::numeric_limits<uint16_t>::max)() ||
      !valid_character(profile.character))
    return false;
  out.push_back(profile.participant);
  out.push_back(static_cast<uint8_t>(profile.character));
  out.push_back(profile.ready ? 1 : 0);
  out.push_back(static_cast<uint8_t>(profile.display_name.size()));
  write_u16(out, static_cast<uint16_t>(profile.game_extension.size()));
  out.insert(out.end(), profile.display_name.begin(), profile.display_name.end());
  out.insert(out.end(), profile.game_extension.begin(), profile.game_extension.end());
  return true;
}
bool decode_profile(const std::span<const uint8_t> bytes,
                    size_t& cursor,
                    const uint16_t maximum_extension_bytes,
                    const uint8_t maximum_participants,
                    ParticipantProfile& profile) {
  uint8_t character = 0;
  uint8_t ready = 0;
  uint8_t name_size = 0;
  uint16_t extension_size = 0;
  if (!read_u8(bytes, cursor, profile.participant) || !read_u8(bytes, cursor, character) ||
      !read_u8(bytes, cursor, ready) || !read_u8(bytes, cursor, name_size) ||
      !read_u16(bytes, cursor, extension_size) || profile.participant >= maximum_participants ||
      ready > 1 || name_size > kMaximumDisplayNameBytes ||
      extension_size > maximum_extension_bytes ||
      bytes.size() - cursor < static_cast<size_t>(name_size) + extension_size)
    return false;
  profile.character = static_cast<PlayerCharacter>(character);
  if (!valid_character(profile.character))
    return false;
  profile.ready = ready != 0;
  profile.display_name.assign(reinterpret_cast<const char*>(bytes.data() + cursor), name_size);
  cursor += name_size;
  profile.game_extension.assign(bytes.begin() + cursor, bytes.begin() + cursor + extension_size);
  cursor += extension_size;
  return true;
}
}  // namespace

bool control_allowed_from(const ControlKind kind, const SessionRole sender) {
  if (sender == SessionRole::HOST) {
    return kind == ControlKind::PROFILE || kind == ControlKind::ROSTER ||
           kind == ControlKind::DEPARTURE || kind == ControlKind::START_COUNTDOWN ||
           kind == ControlKind::CANCEL_COUNTDOWN || kind == ControlKind::START_GAME ||
           kind == ControlKind::SESSION_CLOSE;
  }
  if (sender == SessionRole::CLIENT) {
    return kind == ControlKind::PROFILE || kind == ControlKind::SET_CHARACTER ||
           kind == ControlKind::SET_READY || kind == ControlKind::BOOTSTRAP_ACK;
  }
  return false;
}

std::vector<uint8_t> encode_control_message(const ControlMessage& message,
                                            const uint16_t maximum_extension_bytes) {
  std::vector<uint8_t> out = {static_cast<uint8_t>(message.kind)};
  switch (message.kind) {
    case ControlKind::PROFILE:
      if (!encode_profile(out, message.profile, maximum_extension_bytes))
        return {};
      break;
    case ControlKind::ROSTER:
      if (message.roster.size() > (std::numeric_limits<uint8_t>::max)())
        return {};
      out.push_back(static_cast<uint8_t>(message.roster.size()));
      for (const auto& profile : message.roster) {
        if (!encode_profile(out, profile, maximum_extension_bytes))
          return {};
      }
      break;
    case ControlKind::DEPARTURE:
      if (message.participant == kInvalidPlayerId)
        return {};
      out.push_back(message.participant);
      out.push_back(message.reason);
      break;
    case ControlKind::SET_CHARACTER:
      if (!valid_character(message.character))
        return {};
      out.push_back(static_cast<uint8_t>(message.character));
      break;
    case ControlKind::SET_READY:
      out.push_back(message.ready ? 1 : 0);
      break;
    case ControlKind::START_COUNTDOWN:
    case ControlKind::BOOTSTRAP_ACK:
      write_u32(out, message.value);
      break;
    case ControlKind::SESSION_CLOSE:
      out.push_back(message.reason);
      break;
    case ControlKind::CANCEL_COUNTDOWN:
    case ControlKind::START_GAME:
      break;
  }
  return out;
}

bool decode_control_message(const std::span<const uint8_t> bytes,
                            const uint16_t maximum_extension_bytes,
                            const uint8_t maximum_participants,
                            ControlMessage& message) {
  if (bytes.empty() || maximum_participants < 2)
    return false;
  message = {};
  message.kind = static_cast<ControlKind>(bytes[0]);
  size_t cursor = 1;
  switch (message.kind) {
    case ControlKind::PROFILE:
      if (!decode_profile(bytes, cursor, maximum_extension_bytes, maximum_participants,
                          message.profile))
        return false;
      break;
    case ControlKind::ROSTER: {
      uint8_t count = 0;
      if (!read_u8(bytes, cursor, count) || count > maximum_participants)
        return false;
      message.roster.resize(count);
      for (auto& profile : message.roster) {
        if (!decode_profile(bytes, cursor, maximum_extension_bytes, maximum_participants, profile))
          return false;
      }
      break;
    }
    case ControlKind::DEPARTURE:
      if (!read_u8(bytes, cursor, message.participant) || !read_u8(bytes, cursor, message.reason) ||
          message.participant >= maximum_participants)
        return false;
      break;
    case ControlKind::SET_CHARACTER: {
      uint8_t character = 0;
      if (!read_u8(bytes, cursor, character))
        return false;
      message.character = static_cast<PlayerCharacter>(character);
      if (!valid_character(message.character))
        return false;
      break;
    }
    case ControlKind::SET_READY: {
      uint8_t ready = 0;
      if (!read_u8(bytes, cursor, ready) || ready > 1)
        return false;
      message.ready = ready != 0;
      break;
    }
    case ControlKind::START_COUNTDOWN:
    case ControlKind::BOOTSTRAP_ACK:
      if (!read_u32(bytes, cursor, message.value))
        return false;
      break;
    case ControlKind::SESSION_CLOSE:
      if (!read_u8(bytes, cursor, message.reason))
        return false;
      break;
    case ControlKind::CANCEL_COUNTDOWN:
    case ControlKind::START_GAME:
      break;
    default:
      return false;
  }
  return cursor == bytes.size();
}

std::vector<uint8_t> encode_gameplay_envelope(const uint8_t message_id,
                                              const uint32_t sequence,
                                              const std::span<const uint8_t> payload) {
  if (sequence == 0)
    return {};
  std::vector<uint8_t> out;
  out.reserve(5 + payload.size());
  out.push_back(message_id);
  write_u32(out, sequence);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool decode_gameplay_envelope(const std::span<const uint8_t> bytes, GameplayEnvelope& envelope) {
  if (bytes.size() < 5)
    return false;
  size_t cursor = 0;
  if (!read_u8(bytes, cursor, envelope.message_id) || !read_u32(bytes, cursor, envelope.sequence) ||
      envelope.sequence == 0)
    return false;
  envelope.payload = bytes.subspan(cursor);
  return true;
}

std::vector<uint8_t> encode_bootstrap_envelope(const uint32_t generation,
                                               const std::span<const uint8_t> payload) {
  if (generation == 0 || payload.empty())
    return {};
  std::vector<uint8_t> out;
  out.reserve(4 + payload.size());
  write_u32(out, generation);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool decode_bootstrap_envelope(const std::span<const uint8_t> bytes, BootstrapEnvelope& envelope) {
  if (bytes.size() <= 4)
    return false;
  size_t cursor = 0;
  if (!read_u32(bytes, cursor, envelope.generation) || envelope.generation == 0)
    return false;
  envelope.payload = bytes.subspan(cursor);
  return true;
}

std::vector<uint8_t> encode_client_gate(const ClientGate& gate) {
  std::vector<uint8_t> out(kGateMagic.begin(), kGateMagic.end());
  out.push_back(kClientGateKind);
  if (!write_string(out, gate.game_id) || !write_string(out, gate.compatibility_identity) ||
      !write_string(out, gate.room_code))
    return {};
  return out;
}

bool decode_client_gate(const std::span<const uint8_t> bytes, ClientGate& gate) {
  size_t cursor = 0;
  gate = {};
  return read_gate_header(bytes, kClientGateKind, cursor) &&
         read_string(bytes, cursor, gate.game_id) &&
         read_string(bytes, cursor, gate.compatibility_identity) &&
         read_string(bytes, cursor, gate.room_code) && cursor == bytes.size();
}

std::vector<uint8_t> encode_server_gate(const ServerGate& gate) {
  std::vector<uint8_t> out(kGateMagic.begin(), kGateMagic.end());
  if (gate.accepted) {
    if (gate.participant == kInvalidPlayerId || gate.participant_capacity < 2)
      return {};
    out.push_back(kAcceptedGateKind);
    out.push_back(gate.participant);
    out.push_back(gate.host_participant);
    out.push_back(gate.participant_capacity);
    out.push_back(static_cast<uint8_t>(gate.character));
  } else {
    if (gate.rejection == RejectionReason::NONE)
      return {};
    out.push_back(kRejectedGateKind);
    out.push_back(static_cast<uint8_t>(gate.rejection));
    if (!write_string(out, gate.required_identity))
      return {};
  }
  return out;
}

bool decode_server_gate(const std::span<const uint8_t> bytes, ServerGate& gate) {
  gate = {};
  size_t cursor = 0;
  if (read_gate_header(bytes, kAcceptedGateKind, cursor)) {
    uint8_t character = 0;
    if (!read_u8(bytes, cursor, gate.participant) ||
        !read_u8(bytes, cursor, gate.host_participant) ||
        !read_u8(bytes, cursor, gate.participant_capacity) || !read_u8(bytes, cursor, character) ||
        cursor != bytes.size() || gate.participant == kInvalidPlayerId ||
        gate.participant_capacity < 2)
      return false;
    gate.accepted = true;
    gate.character = static_cast<PlayerCharacter>(character);
    return true;
  }
  cursor = 0;
  uint8_t reason = 0;
  if (!read_gate_header(bytes, kRejectedGateKind, cursor) || !read_u8(bytes, cursor, reason) ||
      reason == static_cast<uint8_t>(RejectionReason::NONE) ||
      reason > static_cast<uint8_t>(RejectionReason::THROTTLED) ||
      !read_string(bytes, cursor, gate.required_identity) || cursor != bytes.size())
    return false;
  gate.rejection = static_cast<RejectionReason>(reason);
  return true;
}

}  // namespace multiplayer::platform
