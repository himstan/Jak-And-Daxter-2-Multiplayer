#pragma once

#include <functional>
#include <stdexcept>

#include "game/multiplayer/platform/session/packet_handler.h"
#include "game/multiplayer/platform/wire/packet_codec.h"

namespace multiplayer::platform {

template <typename Model, wire::SendablePacket Packet>
class TypedPacketHandler : public PacketHandler {
 public:
  struct Hooks {
    Packet (*to_wire)(const Model&);
    void (*canonicalize)(Model&, const MessageOrigin&);
    std::function<bool(const Model&, const PacketContext&)> apply;
    std::function<void(TypedPacketHandler&, GameSessionEndpoint&, uint64_t)> produce;
    std::function<std::optional<bool>()> ready;
    std::function<std::optional<std::vector<PlayerId>>(const GameplayMessage&, const SessionState&)>
        relay;
  };

  TypedPacketHandler(GameMessagePolicy policy, Hooks hooks)
      : PacketHandler(policy), hooks_(std::move(hooks)) {
    if (policy.id != static_cast<uint8_t>(Packet::kPacketType) || !hooks_.to_wire ||
        !hooks_.canonicalize || !hooks_.apply || !hooks_.produce || !hooks_.ready || !hooks_.relay)
      throw std::invalid_argument("incomplete packet handler");
  }

  ValidatedPayload receive(const GameplayMessage& message, GameSessionEndpoint& endpoint) final {
    const auto& session = endpoint.snapshot().state;
    if (message.message_id != policy().id ||
        message.origin.authenticated_player_id >= session.player_limit ||
        message.payload.size() > policy().maximum_payload_bytes)
      return {};
    const auto packet = wire::decode_packet<Packet>(message.payload);
    if (!packet)
      return {};
    Model model = {};
    from_packet(*packet, model);
    hooks_.canonicalize(model, message.origin);
    auto canonical = wire::encode_packet(hooks_.to_wire(model));
    if (!canonical || canonical->size() > policy().maximum_payload_bytes ||
        !hooks_.apply(model, {.sequence = message.sequence,
                              .source = message.origin,
                              .received_at_ms = message.received_at_ms,
                              .local_player_id = session.local_player_id}))
      return {};
    return {.disposition = session.role == SessionRole::HOST ? PayloadDisposition::CONSUME_AND_RELAY
                                                             : PayloadDisposition::CONSUME,
            .canonical_payload = std::move(*canonical),
            .relay_recipients =
                session.role == SessionRole::HOST ? hooks_.relay(message, session) : std::nullopt};
  }

  void publish(GameSessionEndpoint& endpoint, uint64_t now_ms) final {
    const auto dirty = hooks_.ready();
    if (dirty && can_send_message(policy(), endpoint.snapshot().state.role) &&
        endpoint.cadence_due(policy().id, now_ms, *dirty))
      hooks_.produce(*this, endpoint, now_ms);
  }

  std::optional<uint32_t> send(const Model& model,
                               GameSessionEndpoint& endpoint,
                               uint64_t now_ms,
                               bool apply_local = true,
                               const Audience& audience = Audience::everyone()) {
    const auto bytes = wire::encode_packet(hooks_.to_wire(model));
    if (!bytes || bytes->size() > policy().maximum_payload_bytes)
      return std::nullopt;
    const auto sequence = endpoint.send_gameplay(policy().id, audience, *bytes);
    if (sequence && apply_local) {
      const auto& session = endpoint.snapshot().state;
      hooks_.apply(model, {.sequence = *sequence,
                           .source = {.authenticated_player_id = session.local_player_id,
                                      .from_host = session.role == SessionRole::HOST},
                           .received_at_ms = now_ms,
                           .local_player_id = session.local_player_id});
    }
    return sequence;
  }

 private:
  const Hooks hooks_;
};

}  // namespace multiplayer::platform
