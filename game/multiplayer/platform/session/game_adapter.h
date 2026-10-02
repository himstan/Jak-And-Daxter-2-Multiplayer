#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game/multiplayer/platform/core/message_policy.h"
#include "game/multiplayer/platform/core/types.h"
#include "game/multiplayer/platform/protocol/session_protocol.h"
#include "game/multiplayer/platform/session/session_snapshot.h"
#include "game/multiplayer/platform/session/session_state.h"

namespace multiplayer::platform {

struct GameDescriptor {
  std::string game_id;
  std::string compatibility_identity;
  uint32_t maximum_payload_bytes = 0;
  uint8_t maximum_players = 0;
  uint16_t maximum_profile_extension_bytes = 0;
  std::vector<PlayerCharacter> supported_characters;
};

enum class PayloadDisposition : uint8_t {
  REJECT,
  DISCONNECT,
  CONSUME,
  CONSUME_AND_RELAY,
};

enum class PacketApplyResult : uint8_t {
  REJECT,
  ACCEPT,
  CAPACITY_EXCEEDED,
};

struct ValidatedPayload {
  PayloadDisposition disposition = PayloadDisposition::REJECT;
  std::vector<uint8_t> canonical_payload;
  std::optional<std::vector<PlayerId>> relay_recipients;
};

struct GameplayMessage {
  MessageOrigin origin;
  uint8_t message_id = 0;
  uint32_t sequence = 0;
  uint64_t received_at_ms = 0;
  std::span<const uint8_t> payload;
};

struct PacketContext {
  uint32_t sequence = 0;
  MessageOrigin source;
  uint64_t received_at_ms = 0;
  PlayerId local_player_id = kInvalidPlayerId;
};

class PacketRegistry;

class GameSessionEndpoint {
 public:
  virtual ~GameSessionEndpoint() = default;
  virtual std::optional<uint32_t> send_gameplay(uint8_t message_id,
                                                const Audience& audience,
                                                std::span<const uint8_t> payload) = 0;
  virtual bool cadence_due(uint8_t message_id, uint64_t now_ms, bool dirty = true) = 0;
  virtual void request_bootstrap() = 0;
  virtual const SessionSnapshot& snapshot() const = 0;
  virtual uint32_t estimated_rtt_ms(PlayerId player_id) const = 0;
  virtual NetworkPressure network_pressure() const = 0;
  virtual bool severe_pressure_sustained(uint64_t now_ms) const = 0;
};

class GameAdapter {
 public:
  virtual ~GameAdapter() = default;

  virtual const GameDescriptor& descriptor() const = 0;
  virtual PacketRegistry& packets() = 0;
  virtual bool configure_compatibility_identity(std::string) { return false; }
  virtual void installed(GameSessionEndpoint&) {}
  virtual void session_started(const SessionState&) {}
  virtual void session_reset() {}
  virtual void tick(uint64_t now_ms) = 0;
  virtual void stop() = 0;

  virtual bool validate_profile_extension(std::span<const uint8_t>, std::vector<uint8_t>&) {
    return false;
  }
  virtual std::vector<uint8_t> create_bootstrap(PlayerId) { return {}; }
  virtual bool apply_bootstrap(uint32_t, std::span<const uint8_t>) { return false; }
  virtual void player_profile_changed(const PlayerProfile&) {}
  virtual void player_departed(PlayerId) {}
};

}  // namespace multiplayer::platform
