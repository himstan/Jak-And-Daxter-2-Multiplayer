#include <array>

#include "game/multiplayer/platform/session/packet_registry.h"
#include "game/multiplayer/platform/session/typed_packet_handler.h"
#include "gtest/gtest.h"

namespace {
using namespace multiplayer::platform;

struct TestPacket : wire::Packet<TestPacket, uint8_t{37}> {
  uint8_t value = 0;
};

template <typename Stream>
bool serialize_fields(Stream& stream, TestPacket& packet) {
  return wire::serialize_u8(stream, packet.value);
}

bool validate_packet(const TestPacket& packet) {
  return packet.value <= 10;
}

struct MissingValidator : wire::Packet<MissingValidator, uint8_t{38}> {};
static_assert(!wire::SendablePacket<MissingValidator>);
static_assert(wire::SendablePacket<TestPacket>);

struct Model {
  uint8_t value = 0;
  PlayerId source = kInvalidPlayerId;
};

TestPacket to_packet(const Model& model) {
  return {.value = model.value};
}
void from_packet(const TestPacket& packet, Model& model) {
  model.value = packet.value;
}

class Endpoint final : public GameSessionEndpoint {
 public:
  std::optional<uint32_t> send_gameplay(uint8_t id,
                                        const Audience&,
                                        std::span<const uint8_t> payload) override {
    sent_id = id;
    bytes.assign(payload.begin(), payload.end());
    return accept_send ? std::optional<uint32_t>{19} : std::nullopt;
  }
  bool cadence_due(uint8_t id, uint64_t now_ms, bool dirty) override {
    cadence_id = id;
    cadence_time = now_ms;
    cadence_dirty = dirty;
    return due;
  }
  const SessionSnapshot& snapshot() const override { return session; }
  void request_bootstrap() override {}
  uint32_t estimated_rtt_ms(PlayerId) const override { return 0; }
  NetworkPressure network_pressure() const override { return NetworkPressure::NORMAL; }
  bool severe_pressure_sustained(uint64_t) const override { return false; }

  SessionSnapshot session = {
      .state = {.role = SessionRole::HOST, .local_player_id = 0, .player_limit = 8}};
  bool due = true;
  bool accept_send = true;
  uint8_t sent_id = 0;
  uint8_t cadence_id = 0;
  uint64_t cadence_time = 0;
  bool cadence_dirty = false;
  std::vector<uint8_t> bytes;
};

using Handler = TypedPacketHandler<Model, TestPacket>;
constexpr GameMessagePolicy kPolicy = {.id = 37, .name = "TEST", .maximum_payload_bytes = 1};

class PacketHandlersTest : public testing::Test {
 protected:
  std::unique_ptr<Handler> handler(GameMessagePolicy policy = kPolicy) {
    return std::make_unique<Handler>(
        policy, Handler::Hooks{.to_wire = to_packet,
                               .canonicalize =
                                   [](Model& model, const MessageOrigin& origin) {
                                     model.source = origin.authenticated_player_id;
                                   },
                               .apply =
                                   [this](const Model& model, const PacketContext& context) {
                                     ++applications;
                                     applied = model;
                                     last_context = context;
                                     return accept_apply;
                                   },
                               .produce =
                                   [this](Handler& packet, auto& endpoint, uint64_t now_ms) {
                                     sequence = packet.send(outbound, endpoint, now_ms);
                                   },
                               .ready = [this] { return ready; },
                               .relay =
                                   [this](const auto&, const auto&) {
                                     ++relays;
                                     return std::optional{std::vector<PlayerId>{2}};
                                   }});
  }
  PacketRegistry registry() {
    std::vector<std::unique_ptr<PacketHandler>> handlers;
    handlers.push_back(handler());
    return PacketRegistry(std::move(handlers), 1);
  }
  Endpoint endpoint;
  Model outbound = {.value = 4};
  Model applied;
  PacketContext last_context;
  size_t applications = 0;
  size_t relays = 0;
  bool accept_apply = true;
  std::optional<bool> ready = true;
  std::optional<uint32_t> sequence;
};

TEST_F(PacketHandlersTest, NewGameNeutralPacketRoutesWithoutPlatformDispatchChanges) {
  auto packets = registry();
  const std::array<uint8_t, 1> bytes = {7};
  const auto result = packets.receive({.origin = {.authenticated_player_id = 3},
                                       .message_id = 37,
                                       .sequence = 41,
                                       .received_at_ms = 123,
                                       .payload = bytes},
                                      endpoint);
  EXPECT_EQ(result.disposition, PayloadDisposition::CONSUME_AND_RELAY);
  EXPECT_EQ(result.canonical_payload, (std::vector<uint8_t>{7}));
  EXPECT_EQ(result.relay_recipients, (std::optional{std::vector<PlayerId>{2}}));
  EXPECT_EQ(applied.source, 3);
  EXPECT_EQ(last_context.sequence, 41u);
  EXPECT_EQ(last_context.received_at_ms, 123u);
  EXPECT_EQ(last_context.local_player_id, 0);
  EXPECT_EQ(packets.receive({.message_id = 38, .sequence = 1}, endpoint).disposition,
            PayloadDisposition::REJECT);
  EXPECT_EQ(packets.find(38), nullptr);
  EXPECT_EQ(applications, 1u);
}

TEST_F(PacketHandlersTest, InvalidPayloadsAndCanonicalEncodingFailBeforeApplication) {
  auto packets = registry();
  const std::array<uint8_t, 1> invalid = {11};
  EXPECT_EQ(packets
                .receive({.origin = {.authenticated_player_id = 0},
                          .message_id = 37,
                          .sequence = 1,
                          .payload = invalid},
                         endpoint)
                .disposition,
            PayloadDisposition::REJECT);
  EXPECT_EQ(
      packets
          .receive({.origin = {.authenticated_player_id = 0}, .message_id = 37, .sequence = 1},
                   endpoint)
          .disposition,
      PayloadDisposition::REJECT);
  const std::array<uint8_t, 2> oversized = {1, 2};
  EXPECT_EQ(packets
                .receive({.origin = {.authenticated_player_id = 0},
                          .message_id = 37,
                          .sequence = 1,
                          .payload = oversized},
                         endpoint)
                .disposition,
            PayloadDisposition::REJECT);
  const std::array<uint8_t, 1> valid = {1};
  EXPECT_EQ(packets
                .receive({.origin = {.authenticated_player_id = 8},
                          .message_id = 37,
                          .sequence = 1,
                          .payload = valid},
                         endpoint)
                .disposition,
            PayloadDisposition::REJECT);
  EXPECT_EQ(applications, 0u);
  auto hooks = Handler::Hooks{.to_wire = [](const Model&) { return TestPacket{.value = 11}; },
                              .canonicalize = [](Model&, const MessageOrigin&) {},
                              .apply =
                                  [this](const Model&, const PacketContext&) {
                                    ++applications;
                                    return true;
                                  },
                              .produce = [](auto&, auto&, uint64_t) {},
                              .ready = [] { return std::optional{true}; },
                              .relay = [](const auto&, const auto&) { return std::nullopt; }};
  Handler cannot_encode(kPolicy, std::move(hooks));
  EXPECT_EQ(cannot_encode
                .receive({.origin = {.authenticated_player_id = 0},
                          .message_id = 37,
                          .sequence = 1,
                          .payload = valid},
                         endpoint)
                .disposition,
            PayloadDisposition::REJECT);
  EXPECT_EQ(applications, 0u);
}

TEST_F(PacketHandlersTest, ZeroSequenceNeverAppliesOrRelaysAValidPayload) {
  auto packets = registry();
  const std::array<uint8_t, 1> bytes = {7};
  GameplayMessage message = {
      .origin = {.authenticated_player_id = 3}, .message_id = 37, .sequence = 0, .payload = bytes};
  for (size_t attempt = 0; attempt < 2; ++attempt) {
    const auto result = packets.receive(message, endpoint);
    EXPECT_EQ(result.disposition, PayloadDisposition::REJECT);
    EXPECT_TRUE(result.canonical_payload.empty());
    EXPECT_FALSE(result.relay_recipients);
  }
  EXPECT_EQ(applications, 0u);
  EXPECT_EQ(relays, 0u);
  message.sequence = 1;
  EXPECT_EQ(packets.receive(message, endpoint).disposition, PayloadDisposition::CONSUME_AND_RELAY);
  EXPECT_EQ(applications, 1u);
  EXPECT_EQ(relays, 1u);
}

TEST_F(PacketHandlersTest, RegistryRejectsDuplicateIdsInvalidPoliciesAndMissingHooks) {
  std::vector<std::unique_ptr<PacketHandler>> duplicates;
  duplicates.push_back(handler());
  duplicates.push_back(handler());
  EXPECT_THROW(PacketRegistry(std::move(duplicates), 1), std::invalid_argument);
  std::vector<std::unique_ptr<PacketHandler>> invalid;
  auto policy = kPolicy;
  policy.maximum_payload_bytes = 0;
  invalid.push_back(handler(policy));
  EXPECT_THROW(PacketRegistry(std::move(invalid), 1), std::invalid_argument);
  std::vector<std::unique_ptr<PacketHandler>> missing;
  missing.push_back(nullptr);
  EXPECT_THROW(PacketRegistry(std::move(missing), 1), std::invalid_argument);
  EXPECT_THROW(Handler(kPolicy, Handler::Hooks{}), std::invalid_argument);
}

TEST_F(PacketHandlersTest, PublishingHonorsReadinessCadenceAndSendRejection) {
  auto packets = registry();
  ready = std::nullopt;
  packets.publish(endpoint, 10);
  EXPECT_EQ(endpoint.cadence_time, 0u);
  ready = false;
  endpoint.due = false;
  packets.publish(endpoint, 20);
  EXPECT_EQ(endpoint.cadence_id, 37);
  EXPECT_FALSE(endpoint.cadence_dirty);
  EXPECT_EQ(applications, 0u);
  endpoint.due = true;
  outbound.value = 11;
  packets.publish(endpoint, 25);
  EXPECT_FALSE(sequence);
  EXPECT_EQ(endpoint.sent_id, 0);
  EXPECT_EQ(applications, 0u);
  outbound.value = 4;
  endpoint.accept_send = false;
  packets.publish(endpoint, 30);
  EXPECT_FALSE(sequence);
  EXPECT_EQ(applications, 0u);
  endpoint.accept_send = true;
  ready = true;
  packets.publish(endpoint, 40);
  EXPECT_EQ(sequence, 19u);
  EXPECT_EQ(endpoint.sent_id, 37);
  EXPECT_EQ(endpoint.bytes, (std::vector<uint8_t>{4}));
  EXPECT_EQ(applications, 1u);
  EXPECT_TRUE(last_context.source.from_host);
  EXPECT_EQ(last_context.received_at_ms, 40u);
}

}  // namespace
