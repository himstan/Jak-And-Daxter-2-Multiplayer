#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "game/multiplayer/jak2/application/jak2_adapter.h"
#include "game/multiplayer/jak2/application/presentation_runtime.h"
#include "game/multiplayer/jak2/wire/packets/bootstrap_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/enemy_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/game_event_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/gungame_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/pedestrian_state_batch_packet.h"
#include "game/multiplayer/jak2/wire/packets/player_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/player_vehicle_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/traffic_authority_state_packet.h"
#include "game/multiplayer/jak2/wire/packets/vehicle_state_batch_packet.h"
#include "game/multiplayer/platform/session/cadence_scheduler.h"
#include "game/multiplayer/platform/wire/quantization.h"
#include "gtest/gtest.h"

namespace {
using namespace multiplayer;

struct SentGameplay {
  uint8_t id = 0;
  platform::Audience audience;
  std::vector<uint8_t> payload;
};

bool encode_player_packet(const jak2::core::PlayerState& state, std::vector<uint8_t>& bytes) {
  return platform::wire::encode_packet(jak2::wire::to_packet(state), bytes);
}

bool decode_event_packet(const std::span<const uint8_t> bytes, jak2::core::GameEventBatch& batch) {
  const auto packet = platform::wire::decode_packet<jak2::wire::GameEventBatchPacket>(bytes);
  if (!packet) {
    return false;
  }
  jak2::wire::from_packet(*packet, batch);
  return true;
}

bool decode_player_vehicle_packet(const std::span<const uint8_t> bytes,
                                  jak2::core::PlayerVehicleState& state) {
  const auto packet = platform::wire::decode_packet<jak2::wire::PlayerVehicleStatePacket>(bytes);
  if (!packet) {
    return false;
  }
  jak2::wire::from_packet(*packet, state);
  return true;
}

bool encode_enemy_packet(const jak2::core::EnemySnapshot& snapshot, std::vector<uint8_t>& bytes) {
  return platform::wire::encode_packet(jak2::wire::to_packet(snapshot), bytes);
}

class RecordingEndpoint final : public platform::GameSessionEndpoint {
 public:
  std::optional<uint32_t> send_gameplay(const uint8_t id,
                                        const platform::Audience& audience,
                                        const std::span<const uint8_t> payload) override {
    if (!accept_sends || (reject_after && sent.size() >= *reject_after))
      return std::nullopt;
    sent.push_back({.id = id,
                    .audience = audience,
                    .payload = std::vector<uint8_t>(payload.begin(), payload.end())});
    if (++next_sequence == 0)
      ++next_sequence;
    return next_sequence;
  }
  bool cadence_due(uint8_t id, uint64_t now_ms, bool dirty) override {
    cadence_times.push_back(now_ms);
    if (schedule_gungame && id == static_cast<uint8_t>(PacketType::GUNGAME_STATE))
      return cadence_enabled && cadence.due(jak2::wire::GungameStatePacket::kPolicy, now_ms, dirty);
    if (schedule_traffic && id == static_cast<uint8_t>(PacketType::PEDESTRIAN_STATE_BATCH))
      return cadence_enabled &&
             cadence.due(jak2::wire::PedestrianStateBatchPacket::kPolicy, now_ms, dirty);
    if (schedule_traffic && id == static_cast<uint8_t>(PacketType::VEHICLE_STATE_BATCH))
      return cadence_enabled &&
             cadence.due(jak2::wire::VehicleStateBatchPacket::kPolicy, now_ms, dirty);
    return cadence_enabled && dirty;
  }
  platform::NetworkPressure network_pressure() const override {
    return platform::NetworkPressure::NORMAL;
  }
  bool severe_pressure_sustained(uint64_t now_ms) const override {
    last_pressure_time_ms = now_ms;
    return false;
  }
  void request_bootstrap() override { bootstrap_requested = true; }
  const platform::SessionSnapshot& snapshot() const override { return session_snapshot; }
  uint32_t estimated_rtt_ms(platform::PlayerId) const override { return 17; }

  platform::SessionSnapshot session_snapshot = {.state = {.role = platform::SessionRole::HOST,
                                                          .status = platform::SessionStatus::LOBBY,
                                                          .local_player_id = 0,
                                                          .host_player_id = 0,
                                                          .player_limit = 8}};
  bool bootstrap_requested = false;
  bool cadence_enabled = true;
  bool accept_sends = true;
  bool schedule_gungame = false;
  bool schedule_traffic = false;
  platform::CadenceScheduler cadence;
  std::optional<size_t> reject_after;
  uint32_t next_sequence = 0;
  std::vector<uint64_t> cadence_times;
  mutable std::optional<uint64_t> last_pressure_time_ms;
  std::vector<SentGameplay> sent;
};

class TrafficRoutingScenario {
 public:
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  jak2::core::TrafficAuthority authority = {.revision = 1};

  TrafficRoutingScenario(platform::SessionRole role, uint8_t local_id) {
    endpoint.session_snapshot.state.role = role;
    endpoint.session_snapshot.state.local_player_id = local_id;
    endpoint.session_snapshot.state.status = platform::SessionStatus::IN_GAME;
    endpoint.session_snapshot.state.player_limit = 3;
    endpoint.session_snapshot.players = {{.player_id = 0}, {.player_id = 1}, {.player_id = 2}};
    endpoint.session_snapshot.connections = {{.player_id = 1, .network = {.connection_id = 11}},
                                             {.player_id = 2, .network = {.connection_id = 12}}};
    endpoint.schedule_traffic = true;
    authority.assignments[0] = 0;
    authority.assignments[1] = 1;
    authority.assignments[2] = 2;
    adapter.installed(endpoint);
    adapter.session_started(endpoint.session_snapshot.state);
  }

  void publish(uint64_t now_ms, uint8_t selected = jak2::core::kInvalidPlayerId) {
    endpoint.sent.clear();
    const auto local = endpoint.session_snapshot.state.local_player_id;
    if (endpoint.session_snapshot.state.role == platform::SessionRole::CLIENT) {
      const auto packet = platform::wire::encode_packet(jak2::wire::to_packet(authority));
      ASSERT_TRUE(packet);
      ASSERT_NE(
          adapter.packets()
              .receive({.origin = {.authenticated_player_id = 0, .from_host = true},
                        .message_id = static_cast<uint8_t>(PacketType::TRAFFIC_AUTHORITY_STATE),
                        .sequence = authority.revision,
                        .payload = *packet},
                       endpoint)
              .disposition,
          platform::PayloadDisposition::REJECT);
    }
    auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
    frame->local_player_id = local;
    frame->host_player_id = 0;
    frame->selected_traffic_authority =
        selected == jak2::core::kInvalidPlayerId ? authority.assignments[local] : selected;
    frame->traffic_authority = authority;
    frame->sample_time_ms = static_cast<uint32_t>(now_ms);
    frame->players[local].state_ready = true;
    frame->pedestrians.kind = jak2::core::TrafficSnapshot::Kind::PEDESTRIANS;
    frame->vehicles.kind = jak2::core::TrafficSnapshot::Kind::VEHICLES;
    for (uint32_t index = 1; index <= 40; ++index) {
      const auto source_bits = static_cast<uint32_t>(local) << 24;
      frame->pedestrians.pedestrians.push_back(
          {.net_id = jak2::core::kTrafficPedestrianNetIdClass | source_bits | index});
      frame->pedestrians.pedestrians.back().quaternion[3] = 1.0f;
      frame->vehicles.vehicles.push_back(
          {.net_id = jak2::core::kTrafficVehicleNetIdClass | source_bits | index});
      frame->vehicles.vehicles.back().quaternion[3] = 1.0f;
    }
    adapter.mailbox().publish_local_frame(std::move(frame));
    adapter.tick(now_ms);
  }

  void select(uint8_t player_id, uint8_t source, uint32_t sequence) {
    jak2::core::PlayerState player = {.player_id = player_id,
                                      .activity = jak2::core::PlayerActivity::IN_GAME,
                                      .state_ready = true,
                                      .spectator_only = true};
    player.selected_traffic_authority = source;
    const auto packet = platform::wire::encode_packet(jak2::wire::to_packet(player));
    ASSERT_TRUE(packet);
    ASSERT_NE(adapter.packets()
                  .receive({.origin = {.authenticated_player_id = player_id,
                                       .from_host = endpoint.session_snapshot.state.role ==
                                                    platform::SessionRole::CLIENT},
                            .message_id = static_cast<uint8_t>(PacketType::PLAYER_STATE),
                            .sequence = sequence,
                            .received_at_ms = sequence,
                            .payload = *packet},
                           endpoint)
                  .disposition,
              platform::PayloadDisposition::REJECT);
  }

  std::vector<SentGameplay> ambient() const {
    std::vector<SentGameplay> result;
    for (const auto& packet : endpoint.sent)
      if (packet.id == static_cast<uint8_t>(PacketType::PEDESTRIAN_STATE_BATCH) ||
          packet.id == static_cast<uint8_t>(PacketType::VEHICLE_STATE_BATCH))
        result.push_back(packet);
    return result;
  }
};

TEST(Jak2AdapterIntegration, HostTrafficTargetsOnlySubscribersAndKeepsFullPopulation) {
  TrafficRoutingScenario scenario(platform::SessionRole::HOST, 0);
  scenario.authority.assignments[2] = 1;
  scenario.publish(100);
  EXPECT_TRUE(scenario.ambient().empty());
  scenario.authority.assignments[2] = 0;
  ++scenario.authority.revision;
  scenario.publish(101);
  const auto packets = scenario.ambient();
  ASSERT_EQ(packets.size(), 2u);
  for (const auto& packet : packets) {
    EXPECT_EQ(packet.audience.kind, platform::AudienceKind::CONNECTION);
    EXPECT_EQ(packet.audience.connection_id, 12u);
  }
  const auto pedestrians =
      platform::wire::decode_packet<jak2::wire::PedestrianStateBatchPacket>(packets[0].payload);
  const auto vehicles =
      platform::wire::decode_packet<jak2::wire::VehicleStateBatchPacket>(packets[1].payload);
  ASSERT_TRUE(pedestrians);
  ASSERT_TRUE(vehicles);
  EXPECT_EQ(pedestrians->pedestrians.size(), 40u);
  EXPECT_EQ(vehicles->vehicles.size(), 40u);
  EXPECT_EQ(vehicles->sample_time_ms, 101u);
  scenario.authority.assignments[1] = 0;
  ++scenario.authority.revision;
  scenario.publish(167);
  EXPECT_EQ(scenario.ambient().size(), 4u);
  scenario.endpoint.session_snapshot.players.pop_back();
  scenario.publish(233);
  for (const auto& packet : scenario.ambient())
    EXPECT_EQ(packet.audience.connection_id, 11u);
  EXPECT_EQ(scenario.ambient().size(), 2u);
  scenario.authority.assignments[1] = 1;
  ++scenario.authority.revision;
  scenario.publish(299);
  EXPECT_TRUE(scenario.ambient().empty());
  EXPECT_TRUE(std::ranges::any_of(scenario.endpoint.sent, [](const auto& packet) {
    return packet.id == static_cast<uint8_t>(PacketType::PLAYER_STATE);
  }));
}

TEST(Jak2AdapterIntegration, SoloClientTrafficStopsAndResumesForFollowersIncludingHost) {
  TrafficRoutingScenario scenario(platform::SessionRole::CLIENT, 1);
  scenario.publish(100);
  EXPECT_TRUE(scenario.ambient().empty());
  scenario.authority.assignments[2] = 1;
  ++scenario.authority.revision;
  scenario.publish(101);
  ASSERT_EQ(scenario.ambient().size(), 2u);
  for (const auto& packet : scenario.ambient())
    EXPECT_EQ(packet.audience.kind, platform::AudienceKind::EVERYONE);
  scenario.authority.assignments[2] = 2;
  ++scenario.authority.revision;
  scenario.publish(167);
  EXPECT_TRUE(scenario.ambient().empty());
  scenario.authority.assignments[0] = 1;
  ++scenario.authority.revision;
  scenario.publish(168);
  EXPECT_EQ(scenario.ambient().size(), 2u);
  scenario.authority.assignments[1] = 0;
  scenario.authority.assignments[0] = 0;
  ++scenario.authority.revision;
  scenario.publish(234);
  EXPECT_TRUE(scenario.ambient().empty());
}

TEST(Jak2AdapterIntegration, SpectatorInterestStartsAndStopsRootPublication) {
  for (const auto role : {platform::SessionRole::HOST, platform::SessionRole::CLIENT}) {
    const uint8_t root = role == platform::SessionRole::HOST ? 0 : 1;
    TrafficRoutingScenario scenario(role, root);
    scenario.publish(100);
    EXPECT_TRUE(scenario.ambient().empty());
    scenario.select(2, root, 1);
    scenario.publish(101);
    EXPECT_EQ(scenario.ambient().size(), 2u);
    scenario.select(2, 2, 2);
    scenario.publish(167);
    EXPECT_TRUE(scenario.ambient().empty());
  }
}

TEST(Jak2AdapterIntegration, TrafficRelayIncludesSpectatorsWithoutSendingToOtherRoots) {
  TrafficRoutingScenario scenario(platform::SessionRole::HOST, 0);
  scenario.publish(100, 1);
  scenario.select(2, 1, 1);
  const auto snapshot = jak2::core::TrafficSnapshot{
      .kind = jak2::core::TrafficSnapshot::Kind::VEHICLES, .authority_revision = 1};
  const auto packet =
      platform::wire::encode_packet(jak2::wire::to_vehicle_state_batch_packet(snapshot));
  ASSERT_TRUE(packet);
  const auto relay = [&](uint32_t sequence) {
    return scenario.adapter.packets().receive(
        {.origin = {.authenticated_player_id = 1},
         .message_id = static_cast<uint8_t>(PacketType::VEHICLE_STATE_BATCH),
         .sequence = sequence,
         .payload = *packet},
        scenario.endpoint);
  };
  auto result = relay(1);
  ASSERT_TRUE(result.relay_recipients);
  EXPECT_EQ(*result.relay_recipients, (std::vector<platform::PlayerId>{2}));
  scenario.adapter.tick(108);
  const auto remote = scenario.adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->selected_traffic.source_player_id, 1u);
  scenario.select(2, 2, 2);
  result = relay(2);
  ASSERT_TRUE(result.relay_recipients);
  EXPECT_TRUE(result.relay_recipients->empty());
}

TEST(Jak2AdapterIntegration, AdapterRejectsMalformedGameplayBeforeRelay) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  const std::vector<uint8_t> malformed = {1};
  const auto result =
      adapter.packets().receive({.origin = {.connection_id = 3, .authenticated_player_id = 1},
                                 .message_id = static_cast<uint8_t>(PacketType::PLAYER_STATE),
                                 .sequence = 1,
                                 .payload = malformed},
                                endpoint);
  EXPECT_EQ(result.disposition, platform::PayloadDisposition::REJECT);
  EXPECT_TRUE(result.canonical_payload.empty());
}

TEST(Jak2AdapterIntegration, AdapterCanonicalizesValidatedPayloadBodiesForRelay) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  jak2::core::PlayerState player = {};
  player.player_id = 1;
  player.position = {1.0f, 2.0f, 3.0f};
  std::vector<uint8_t> body;
  ASSERT_TRUE(encode_player_packet(player, body));
  const uint64_t received_at_ms = (uint64_t{1} << 32) + 1234;
  const auto result =
      adapter.packets().receive({.origin = {.connection_id = 3, .authenticated_player_id = 1},
                                 .message_id = static_cast<uint8_t>(PacketType::PLAYER_STATE),
                                 .sequence = 7,
                                 .received_at_ms = received_at_ms,
                                 .payload = body},
                                endpoint);
  EXPECT_EQ(result.disposition, platform::PayloadDisposition::CONSUME_AND_RELAY);
  EXPECT_EQ(result.canonical_payload, (std::vector<uint8_t>(body.begin(), body.end())));
  adapter.tick(received_at_ms + 66);
  const auto remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->players[1].received_time_ms, received_at_ms);
}

TEST(Jak2AdapterIntegration, RejectedStaleAndSemanticPayloadsDoNotMutateAcceptedState) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  jak2::core::PlayerState player = {};
  player.player_id = 1;
  player.position = {1.0f, 2.0f, 3.0f};
  std::vector<uint8_t> body;
  ASSERT_TRUE(encode_player_packet(player, body));
  ASSERT_EQ(adapter.packets()
                .receive({.origin = {.connection_id = 3, .authenticated_player_id = 1},
                          .message_id = static_cast<uint8_t>(PacketType::PLAYER_STATE),
                          .sequence = 2,
                          .payload = body},
                         endpoint)
                .disposition,
            platform::PayloadDisposition::CONSUME_AND_RELAY);

  player.position = {9.0f, 8.0f, 7.0f};
  ASSERT_TRUE(encode_player_packet(player, body));
  const auto stale =
      adapter.packets().receive({.origin = {.connection_id = 3, .authenticated_player_id = 1},
                                 .message_id = static_cast<uint8_t>(PacketType::PLAYER_STATE),
                                 .sequence = 1,
                                 .payload = body},
                                endpoint);
  EXPECT_EQ(stale.disposition, platform::PayloadDisposition::REJECT);
  EXPECT_TRUE(stale.canonical_payload.empty());

  jak2::core::TrafficSnapshot unauthorized_traffic = {
      .kind = jak2::core::TrafficSnapshot::Kind::VEHICLES,
      .source_player_id = 1,
      .authority_revision = 1,
      .vehicles = {{.net_id = 0x21000001u, .quaternion = {0.0f, 0.0f, 0.0f, 1.0f}}}};
  const auto encoded_traffic = platform::wire::encode_packet(
      jak2::wire::to_vehicle_state_batch_packet(unauthorized_traffic));
  ASSERT_TRUE(encoded_traffic);
  const auto semantically_rejected = adapter.packets().receive(
      {.origin = {.connection_id = 3, .authenticated_player_id = 1},
       .message_id = static_cast<uint8_t>(PacketType::VEHICLE_STATE_BATCH),
       .sequence = 3,
       .payload = *encoded_traffic},
      endpoint);
  EXPECT_EQ(semantically_rejected.disposition, platform::PayloadDisposition::REJECT);
  EXPECT_TRUE(semantically_rejected.canonical_payload.empty());

  adapter.tick(100);
  const auto remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_NEAR(remote->players[1].position[0], 1.0f, platform::wire::kPositionResolution * 0.5f);
  EXPECT_EQ(remote->players[1].last_sequence, 2u);
}

TEST(Jak2AdapterIntegration, TrafficRelayTargetsOnlyPlayersAssignedToItsSource) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  endpoint.session_snapshot.state.player_limit = 4;
  endpoint.session_snapshot.players = {{.player_id = 0}, {.player_id = 1}, {.player_id = 2}};
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);

  auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
  frame->local_player_id = 0;
  frame->host_player_id = 0;
  frame->traffic_authority.revision = 1;
  frame->traffic_authority.assignments.fill(jak2::core::kInvalidPlayerId);
  frame->traffic_authority.assignments[0] = 0;
  frame->traffic_authority.assignments[1] = 1;
  frame->traffic_authority.assignments[2] = 1;
  adapter.mailbox().publish_local_frame(std::move(frame));
  adapter.tick(100);

  jak2::core::TrafficSnapshot traffic = {.kind = jak2::core::TrafficSnapshot::Kind::VEHICLES,
                                         .source_player_id = 1,
                                         .authority_revision = 1,
                                         .vehicles = {{.net_id = 0x21000001u}}};
  traffic.vehicles[0].quaternion[3] = 1.0f;
  const auto encoded =
      platform::wire::encode_packet(jak2::wire::to_vehicle_state_batch_packet(traffic));
  ASSERT_TRUE(encoded);
  const auto result = adapter.packets().receive(
      {.origin = {.connection_id = 3, .authenticated_player_id = 1},
       .message_id = static_cast<uint8_t>(PacketType::VEHICLE_STATE_BATCH),
       .sequence = 1,
       .received_at_ms = 110,
       .payload = *encoded},
      endpoint);
  EXPECT_EQ(result.disposition, platform::PayloadDisposition::CONSUME_AND_RELAY);
  ASSERT_TRUE(result.relay_recipients);
  EXPECT_EQ(*result.relay_recipients, (std::vector<platform::PlayerId>{2}));
  EXPECT_EQ(result.canonical_payload, *encoded);
}

TEST(Jak2AdapterIntegration, LocalFrameSendsThroughRecordingSessionEndpoint) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  endpoint.next_sequence = 40;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
  frame->local_player_id = 0;
  frame->host_player_id = 0;
  frame->players[0].player_id = 0;
  frame->players[0].state_ready = true;
  adapter.mailbox().publish_local_frame(std::move(frame));
  adapter.tick(100);
  ASSERT_FALSE(endpoint.sent.empty());
  EXPECT_EQ(endpoint.sent.front().id, static_cast<uint8_t>(PacketType::PLAYER_STATE));
  EXPECT_FALSE(endpoint.sent.front().payload.empty());
  const auto remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->players[0].last_sequence, 41u);
}

TEST(Jak2AdapterIntegration, BootstrapPayloadFitsAllPermanentAids) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
  frame->local_player_id = 0;
  frame->host_player_id = 0;
  frame->players[0].player_id = 0;
  frame->players[0].state_ready = true;
  frame->bootstrap.host_continue[0] = 'a';
  frame->bootstrap.synchronized_aid_count = jak2::core::kMaxBootstrapAids;
  for (size_t index = 0; index < jak2::core::kMaxBootstrapAids; ++index) {
    frame->bootstrap.synchronized_aids[index] = 0x10000000u + static_cast<uint32_t>(index);
  }
  adapter.mailbox().publish_local_frame(std::move(frame));
  adapter.tick(100);

  const auto payload = adapter.create_bootstrap(1);
  EXPECT_GT(payload.size(), 8192u);
  EXPECT_LE(payload.size(), adapter.descriptor().maximum_payload_bytes);
  const auto decoded = platform::wire::decode_packet<jak2::wire::BootstrapStatePacket>(payload);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->synchronized_aids.size(), jak2::core::kMaxBootstrapAids);
  EXPECT_EQ(decoded->synchronized_aids.back(), 0x10000fffu);
}

TEST(Jak2AdapterIntegration, LocalTimersKeep64BitTimeAcross32BitBoundary) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
  frame->local_player_id = 0;
  frame->host_player_id = 0;
  frame->players[0].player_id = 0;
  frame->players[0].state_ready = true;
  adapter.mailbox().publish_local_frame(std::move(frame));

  const uint64_t before_wrap = static_cast<uint64_t>(UINT32_MAX) - 3;
  adapter.tick(before_wrap);
  ASSERT_FALSE(endpoint.cadence_times.empty());
  for (const auto time : endpoint.cadence_times)
    EXPECT_EQ(time, before_wrap);
  ASSERT_TRUE(endpoint.last_pressure_time_ms);
  EXPECT_EQ(*endpoint.last_pressure_time_ms, before_wrap);
  ASSERT_TRUE(adapter.mailbox().take_remote_frame());

  endpoint.cadence_times.clear();
  adapter.tick(before_wrap + 8);
  ASSERT_FALSE(endpoint.cadence_times.empty());
  for (const auto time : endpoint.cadence_times)
    EXPECT_EQ(time, before_wrap + 8);
  EXPECT_EQ(*endpoint.last_pressure_time_ms, before_wrap + 8);
  EXPECT_TRUE(adapter.mailbox().take_remote_frame());
}

TEST(Jak2AdapterIntegration, InboundEventOverflowRequestsDisconnectAndDrainingRecovers) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  jak2::core::GameEventBatch batch;
  batch.events.assign(255, {.event_id = 12});
  const auto bytes = platform::wire::encode_packet(jak2::wire::to_packet(batch));
  ASSERT_TRUE(bytes);
  const auto receive = [&](uint32_t sequence) {
    return adapter.packets()
        .receive({.origin = {.authenticated_player_id = 1},
                  .message_id = static_cast<uint8_t>(PacketType::GAME_EVENT_BATCH),
                  .sequence = sequence,
                  .payload = *bytes},
                 endpoint)
        .disposition;
  };
  for (uint32_t sequence = 1; sequence <= 4; ++sequence)
    ASSERT_EQ(receive(sequence), platform::PayloadDisposition::CONSUME_AND_RELAY);
  EXPECT_EQ(receive(5), platform::PayloadDisposition::DISCONNECT);
  adapter.tick(100);
  EXPECT_EQ(adapter.mailbox().inbound_event_count(), jak2::application::kReplicationEventCapacity);
  EXPECT_EQ(receive(5), platform::PayloadDisposition::CONSUME_AND_RELAY);
  adapter.session_reset();
  EXPECT_EQ(adapter.mailbox().inbound_event_count(), 0u);
  EXPECT_EQ(receive(1), platform::PayloadDisposition::CONSUME_AND_RELAY);
}

TEST(Jak2AdapterIntegration, AdapterEventPublisherPreservesEventBatchContents) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  endpoint.session_snapshot.state.local_player_id = 3;
  endpoint.session_snapshot.state.host_player_id = 3;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);

  std::vector<jak2::core::GameEvent> events(2);
  events[0].event_id = 1;
  events[0].payload_size = 4;
  events[1].event_id = 2;
  events[1].payload_size = 4;
  ASSERT_TRUE(adapter.mailbox().push_outbound_events(events));
  adapter.tick(100);

  ASSERT_EQ(endpoint.sent.size(), 1u);
  jak2::core::GameEventBatch decoded;
  ASSERT_TRUE(decode_event_packet(endpoint.sent[0].payload, decoded));
  ASSERT_EQ(decoded.events.size(), 2u);
  EXPECT_EQ(decoded.events[0].event_id, 1u);
  EXPECT_EQ(decoded.events[1].event_id, 2u);
}

TEST(Jak2AdapterIntegration, AdapterEventPublisherSplitsBatchesAtPayloadLimit) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  endpoint.session_snapshot.state.local_player_id = 3;
  endpoint.session_snapshot.state.host_player_id = 3;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);

  std::vector<jak2::core::GameEvent> events(125);
  for (auto& event : events) {
    event.event_id = 6;
    event.payload_size = 64;
  }
  ASSERT_TRUE(adapter.mailbox().push_outbound_events(events));
  adapter.tick(100);

  ASSERT_EQ(endpoint.sent.size(), 2u);
  jak2::core::GameEventBatch first;
  jak2::core::GameEventBatch second;
  ASSERT_TRUE(decode_event_packet(endpoint.sent[0].payload, first));
  ASSERT_TRUE(decode_event_packet(endpoint.sent[1].payload, second));
  EXPECT_EQ(first.events.size(), 124u);
  EXPECT_EQ(second.events.size(), 1u);
  EXPECT_LE(endpoint.sent[0].payload.size(), 8192u);
  EXPECT_LE(endpoint.sent[1].payload.size(), 8192u);
  EXPECT_EQ(adapter.mailbox().outbound_event_count(), 0u);
}

TEST(Jak2AdapterIntegration, AdapterEventPublisherRestoresEventsWhenPlatformRejectsSend) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  endpoint.session_snapshot.state.local_player_id = 3;
  endpoint.session_snapshot.state.host_player_id = 3;
  endpoint.accept_sends = false;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);

  std::vector<jak2::core::GameEvent> events(1);
  events[0].event_id = 9;
  ASSERT_TRUE(adapter.mailbox().push_outbound_events(events));
  adapter.tick(100);
  EXPECT_TRUE(endpoint.sent.empty());
  EXPECT_EQ(adapter.mailbox().outbound_event_count(), 1u);
}

TEST(Jak2AdapterIntegration, EventPublisherRestoresOnlyTheBatchThatFailedAndItsRemainingEvents) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  endpoint.reject_after = 1;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  std::vector<jak2::core::GameEvent> events(125);
  for (auto& event : events) {
    event.event_id = 6;
    event.payload_size = 64;
  }
  events.back().payload[0] = 0x7e;
  ASSERT_TRUE(adapter.mailbox().push_outbound_events(events));
  adapter.tick(100);
  ASSERT_EQ(endpoint.sent.size(), 1u);
  EXPECT_EQ(adapter.mailbox().outbound_event_count(), 1u);
  jak2::core::GameEventBatch accepted;
  ASSERT_TRUE(decode_event_packet(endpoint.sent[0].payload, accepted));
  EXPECT_EQ(accepted.events.size(), 124u);

  endpoint.reject_after.reset();
  adapter.tick(108);
  ASSERT_EQ(endpoint.sent.size(), 2u);
  jak2::core::GameEventBatch retried;
  ASSERT_TRUE(decode_event_packet(endpoint.sent[1].payload, retried));
  ASSERT_EQ(retried.events.size(), 1u);
  EXPECT_EQ(retried.events[0].payload[0], 0x7e);
  EXPECT_EQ(adapter.mailbox().outbound_event_count(), 0u);
}

TEST(Jak2AdapterIntegration, RejectedTrafficAuthorityStaysDirtyUntilSubmissionSucceeds) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  endpoint.accept_sends = false;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
  frame->local_player_id = 0;
  frame->host_player_id = 0;
  frame->traffic_authority.revision = 1;
  frame->traffic_authority.assignments.fill(0);
  frame->selected_traffic_authority = 0;
  adapter.mailbox().publish_local_frame(std::move(frame));
  adapter.tick(100);
  auto remote = adapter.mailbox().take_remote_frame();
  ASSERT_NE(remote, nullptr);
  EXPECT_EQ(remote->traffic_authority.revision, 0u);
  EXPECT_TRUE(endpoint.sent.empty());

  endpoint.accept_sends = true;
  adapter.tick(108);
  remote = adapter.mailbox().take_remote_frame();
  ASSERT_NE(remote, nullptr);
  EXPECT_EQ(remote->traffic_authority.revision, 1u);
  const auto authority_sends = [&] {
    return std::ranges::count_if(endpoint.sent, [](const auto& packet) {
      return packet.id == static_cast<uint8_t>(PacketType::TRAFFIC_AUTHORITY_STATE);
    });
  };
  EXPECT_EQ(authority_sends(), 1);
  adapter.tick(116);
  EXPECT_EQ(authority_sends(), 1);
}

TEST(Jak2AdapterIntegration, RejectedPeriodicSendDoesNotMutateLocalReplicationState) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  endpoint.accept_sends = false;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
  frame->local_player_id = 0;
  frame->host_player_id = 0;
  frame->players[0] = {.player_id = 0, .state_ready = true, .state_id = 17};
  adapter.mailbox().publish_local_frame(std::move(frame));
  adapter.tick(100);
  const auto remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->players[0].last_sequence, 0u);
  EXPECT_EQ(remote->players[0].state_id, 0u);
}

TEST(Jak2AdapterIntegration, RemoteFramesPublishImmediatelyThenAtEightMillisecondIntervals) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);

  adapter.tick(100);
  const auto first = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(first);

  jak2::core::GameEventBatch inbound_events = {
      .events = {{.event_id = 1, .source_player_id = 1, .payload_size = 4}}};
  const auto encoded_events = platform::wire::encode_packet(jak2::wire::to_packet(inbound_events));
  ASSERT_TRUE(encoded_events);
  ASSERT_EQ(adapter.packets()
                .receive({.origin = {.connection_id = 3, .authenticated_player_id = 1},
                          .message_id = static_cast<uint8_t>(PacketType::GAME_EVENT_BATCH),
                          .sequence = 1,
                          .payload = *encoded_events},
                         endpoint)
                .disposition,
            platform::PayloadDisposition::CONSUME_AND_RELAY);

  auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
  frame->local_player_id = 0;
  frame->host_player_id = 0;
  frame->players[0] = {.player_id = 0, .state_ready = true, .state_id = 17};
  adapter.mailbox().publish_local_frame(std::move(frame));
  adapter.tick(101);
  EXPECT_FALSE(adapter.mailbox().take_remote_frame());
  EXPECT_FALSE(endpoint.sent.empty());
  const auto published_events = adapter.mailbox().take_inbound_events(1);
  ASSERT_EQ(published_events.size(), 1u);
  EXPECT_EQ(published_events.front().event_id, 1u);

  adapter.tick(107);
  EXPECT_FALSE(adapter.mailbox().take_remote_frame());
  adapter.tick(108);
  const auto second = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(second);
  EXPECT_GT(second->generation, first->generation);
  EXPECT_EQ(second->players[0].state_id, 17u);
}

TEST(Jak2AdapterIntegration, StaleGoalIdentityFrameIsSkipped) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
  frame->local_player_id = 1;
  frame->host_player_id = 0;
  frame->players[1] = {.player_id = 1, .state_ready = true};
  adapter.mailbox().publish_local_frame(std::move(frame));
  adapter.tick(100);
  EXPECT_TRUE(endpoint.sent.empty());
}

TEST(Jak2AdapterIntegration, LocalAirlocksAreSentWithoutLoopbackMutation) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
  frame->local_player_id = 0;
  frame->host_player_id = 0;
  frame->airlocks.states = {{.airlock_aid = 7, .state_id = 1}};
  adapter.mailbox().publish_local_frame(std::move(frame));
  adapter.tick(100);
  EXPECT_NE(std::ranges::find_if(endpoint.sent,
                                 [](const auto& sent) {
                                   return sent.id ==
                                          static_cast<uint8_t>(PacketType::AIRLOCK_STATE_BATCH);
                                 }),
            endpoint.sent.end());
  const auto remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  for (const auto& snapshot : remote->airlocks)
    EXPECT_TRUE(snapshot.states.empty());
}

TEST(Jak2AdapterIntegration, SessionResetClearsStateAndMailbox) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
  frame->local_player_id = 0;
  frame->host_player_id = 0;
  adapter.mailbox().publish_local_frame(std::move(frame));
  ASSERT_TRUE(
      adapter.mailbox().push_outbound_events(std::vector<jak2::core::GameEvent>{{.event_id = 1}}));
  adapter.session_reset();
  EXPECT_FALSE(adapter.mailbox().take_local_frame());
  EXPECT_EQ(adapter.mailbox().outbound_event_count(), 0u);
  adapter.tick(100);
  const auto remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->world.clock, 0u);
}

TEST(Jak2AdapterIntegration, PlayerVehiclePacketsUseGameFrameCaptureTime) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
  frame->sample_time_ms = 77;
  frame->local_player_id = 0;
  frame->host_player_id = 0;
  frame->players[0].player_id = 0;
  frame->players[0].state_ready = true;
  frame->players[0].vehicle_id = 42;
  frame->players[0].vehicle_seat = 0;
  frame->player_vehicle = jak2::core::PlayerVehicleState{
      .player_id = 0,
      .seat_index = 0,
      .vehicle = {.net_id = 42, .quaternion = {0.0f, 0.0f, 0.0f, 1.0f}}};
  adapter.mailbox().publish_local_frame(std::move(frame));

  adapter.tick(100);

  const auto packet =
      std::find_if(endpoint.sent.begin(), endpoint.sent.end(), [](const auto& sent) {
        return sent.id == static_cast<uint8_t>(PacketType::PLAYER_VEHICLE_STATE);
      });
  ASSERT_NE(packet, endpoint.sent.end());
  jak2::core::PlayerVehicleState decoded = {};
  ASSERT_TRUE(decode_player_vehicle_packet(packet->payload, decoded));
  EXPECT_EQ(decoded.vehicle.sample_time_ms, 77u);
}

TEST(Jak2AdapterIntegration, PresentationTimelinePredictsBeyondTheRawPlayerTransform) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  adapter.tick(1000);

  jak2::core::PlayerState player = {};
  player.player_id = 1;
  player.activity = multiplayer::jak2::core::PlayerActivity::LOBBY;
  player.state_ready = true;
  player.levels[0].level_id = 7;
  player.sample_time_ms = 900;
  player.position = {0.0f, 0.0f, 0.0f};
  player.velocity = {10.0f, 0.0f, 0.0f};
  std::vector<uint8_t> body;
  ASSERT_TRUE(encode_player_packet(player, body));
  ASSERT_EQ(adapter.packets()
                .receive({.origin = {.connection_id = 3, .authenticated_player_id = 1},
                          .message_id = static_cast<uint8_t>(PacketType::PLAYER_STATE),
                          .sequence = 1,
                          .payload = body},
                         endpoint)
                .disposition,
            platform::PayloadDisposition::CONSUME_AND_RELAY);
  adapter.tick(1033);

  player.sample_time_ms = 933;
  player.position[0] = 0.33f;
  ASSERT_TRUE(encode_player_packet(player, body));
  ASSERT_EQ(adapter.packets()
                .receive({.origin = {.connection_id = 3, .authenticated_player_id = 1},
                          .message_id = static_cast<uint8_t>(PacketType::PLAYER_STATE),
                          .sequence = 2,
                          .payload = body},
                         endpoint)
                .disposition,
            platform::PayloadDisposition::CONSUME_AND_RELAY);
  adapter.tick(1100);
  const auto remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  ASSERT_TRUE(remote->player_targets[1].valid);
  EXPECT_NEAR(remote->players[1].position[0], 0.33f, platform::wire::kPositionResolution * 0.5f);
  EXPECT_GT(remote->player_targets[1].position[0], remote->players[1].position[0]);
  EXPECT_NEAR(remote->player_targets[1].velocity[0], 10.0f,
              platform::wire::kLinearVelocityResolution * 0.5f);
}

TEST(Jak2AdapterIntegration, BulkBridgeGenerationsStayStableWithoutNewPackets) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);

  auto local = std::make_unique<jak2::application::LocalReplicationFrame>();
  local->local_player_id = 0;
  local->host_player_id = 0;
  local->players[0].player_id = 0;
  local->players[0].state_ready = true;
  local->traffic_authority.revision = 1;
  local->traffic_authority.assignments[0] = 0;
  local->selected_traffic_authority = 0;
  local->pedestrians.kind = jak2::core::TrafficSnapshot::Kind::PEDESTRIANS;
  local->pedestrians.pedestrians.push_back({.net_id = 11});
  local->vehicles.kind = jak2::core::TrafficSnapshot::Kind::VEHICLES;
  local->vehicles.vehicles.push_back({.net_id = 12});
  adapter.mailbox().publish_local_frame(std::move(local));
  adapter.tick(2000);
  const auto first = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(first);
  ASSERT_GT(first->traffic_generation, 0u);

  endpoint.cadence_enabled = false;
  adapter.tick(2010);
  const auto second = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(second);
  EXPECT_EQ(second->traffic_generation, first->traffic_generation);

  jak2::core::EnemySnapshot enemies = {.source_player_id = 1, .sample_time_ms = 1900};
  enemies.enemies.push_back({.actor_id = 99, .owner_player_id = 1});
  std::vector<uint8_t> body;
  ASSERT_TRUE(encode_enemy_packet(enemies, body));
  ASSERT_EQ(adapter.packets()
                .receive({.origin = {.connection_id = 3, .authenticated_player_id = 1},
                          .message_id = static_cast<uint8_t>(PacketType::ENEMY_STATE_BATCH),
                          .sequence = 1,
                          .payload = body},
                         endpoint)
                .disposition,
            platform::PayloadDisposition::CONSUME_AND_RELAY);
  adapter.tick(2020);
  const auto third = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(third);
  EXPECT_GT(third->enemy_generation, second->enemy_generation);
  adapter.tick(2030);
  const auto fourth = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(fourth);
  EXPECT_EQ(fourth->enemy_generation, third->enemy_generation);
}

TEST(Jak2AdapterIntegration, PresentationRuntimeKeepsAuthoritativeSnapshotsUnmodified) {
  auto frame = std::make_unique<jak2::application::RemoteReplicationFrame>();
  frame->players[0] = {.player_id = 0,
                       .state_ready = true,
                       .position = {1.0f, 2.0f, 3.0f},
                       .velocity = {4.0f, 0.0f, 0.0f},
                       .sample_time_ms = 100,
                       .last_sequence = 1,
                       .received_time_ms = 150};
  frame->players[0].levels[0].level_id = 7;
  frame->player_vehicles[0] = {.player_id = 0,
                               .vehicle = {.net_id = 0x40000001,
                                           .position = {5.0f, 6.0f, 7.0f},
                                           .quaternion = {0.0f, 0.0f, 0.0f, 1.0f},
                                           .linear_velocity = {8.0f, 0.0f, 0.0f},
                                           .sample_time_ms = 100,
                                           .last_sequence = 1,
                                           .received_time_ms = 150}};
  frame->enemies.enemies.push_back({.actor_id = 21,
                                    .owner_player_id = 0,
                                    .position = {9.0f, 10.0f, 11.0f},
                                    .quaternion = {0.0f, 0.0f, 0.0f, 1.0f},
                                    .sample_time_ms = 100,
                                    .received_time_ms = 150,
                                    .last_sequence = 1});
  frame->selected_traffic.kind = jak2::core::TrafficSnapshot::Kind::PEDESTRIANS;
  frame->selected_traffic.source_player_id = 0;
  frame->selected_traffic.authority_revision = 4;
  frame->selected_traffic.level_id = 7;
  frame->selected_traffic.pedestrians.push_back({.net_id = 31,
                                                 .position = {12.0f, 13.0f, 14.0f},
                                                 .quaternion = {0.0f, 0.0f, 0.0f, 1.0f},
                                                 .sample_time_ms = 100,
                                                 .last_sequence = 1,
                                                 .received_time_ms = 150});
  frame->selected_traffic.vehicles.push_back({.net_id = 41,
                                              .position = {15.0f, 16.0f, 17.0f},
                                              .quaternion = {0.0f, 0.0f, 0.0f, 1.0f},
                                              .linear_velocity = {18.0f, 0.0f, 0.0f},
                                              .sample_time_ms = 100,
                                              .last_sequence = 1,
                                              .received_time_ms = 150});
  frame->bosses[0] = {.kind = jak2::core::BossState::Kind::PALACE_SQUID,
                      .active = 1,
                      .stage = 2,
                      .root_position = {19.0f, 20.0f, 21.0f},
                      .root_quaternion = {0.0f, 0.0f, 0.0f, 1.0f},
                      .sample_time_ms = 100,
                      .received_time_ms = 150,
                      .sequence = 1};
  frame->bosses[1] = {.kind = jak2::core::BossState::Kind::WIDOW,
                      .active = 1,
                      .position = {22.0f, 23.0f, 24.0f},
                      .quaternion = {0.0f, 0.0f, 0.0f, 1.0f},
                      .sample_time_ms = 100,
                      .received_time_ms = 150,
                      .sequence = 1};

  const auto original_player_position = frame->players[0].position;
  const auto original_vehicle = frame->player_vehicles[0].vehicle;
  const auto original_enemies = frame->enemies;
  const auto original_traffic = frame->selected_traffic;
  const auto original_squid_root = frame->bosses[0].root_position;
  const auto original_widow_position = frame->bosses[1].position;

  jak2::core::ReplicationState core;
  jak2::application::PresentationRuntime presentation;
  presentation.prepare(*frame, core, 183);

  EXPECT_EQ(frame->players[0].position, original_player_position);
  EXPECT_EQ(frame->player_vehicles[0].vehicle, original_vehicle);
  EXPECT_EQ(frame->enemies, original_enemies);
  EXPECT_EQ(frame->selected_traffic, original_traffic);
  EXPECT_EQ(frame->bosses[0].root_position, original_squid_root);
  EXPECT_EQ(frame->bosses[1].position, original_widow_position);
  EXPECT_TRUE(frame->player_targets[0].valid);
  EXPECT_TRUE(frame->player_vehicle_targets[0].valid);
  ASSERT_EQ(frame->enemy_targets.size(), 1u);
  ASSERT_EQ(frame->pedestrian_targets.size(), 1u);
  ASSERT_EQ(frame->ambient_vehicle_targets.size(), 1u);
  EXPECT_TRUE(frame->enemy_targets[0].valid);
  EXPECT_TRUE(frame->pedestrian_targets[0].valid);
  EXPECT_TRUE(frame->ambient_vehicle_targets[0].valid);
  EXPECT_TRUE(frame->boss_targets[0].valid);
  EXPECT_TRUE(frame->boss_targets[1].valid);
  EXPECT_EQ(frame->enemy_targets[0].entity_id, 21u);
  EXPECT_EQ(frame->pedestrian_targets[0].entity_id, 31u);
  EXPECT_EQ(frame->ambient_vehicle_targets[0].entity_id, 41u);
  EXPECT_EQ(frame->boss_targets[0].position, original_squid_root);
  EXPECT_EQ(frame->boss_targets[1].position, original_widow_position);

  const auto player_generation = frame->player_targets[0].generation;
  const auto player_x = frame->player_targets[0].position[0];
  const auto vehicle_generation = frame->player_vehicle_targets[0].generation;
  const auto vehicle_x = frame->player_vehicle_targets[0].position[0];
  const auto ambient_generation = frame->ambient_vehicle_targets[0].generation;
  const auto ambient_x = frame->ambient_vehicle_targets[0].position[0];
  presentation.prepare(*frame, core, 249);
  EXPECT_EQ(frame->player_targets[0].generation, player_generation);
  EXPECT_EQ(frame->player_vehicle_targets[0].generation, vehicle_generation);
  EXPECT_EQ(frame->ambient_vehicle_targets[0].generation, ambient_generation);
  EXPECT_GT(frame->player_targets[0].position[0], player_x);
  EXPECT_GT(frame->player_vehicle_targets[0].position[0], vehicle_x);
  EXPECT_GT(frame->ambient_vehicle_targets[0].position[0], ambient_x);
}

TEST(Jak2AdapterIntegration, ProfileExtensionAndPlayerLifecycleStayGameSpecific) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  std::vector<uint8_t> extension(sizeof(jak2::core::PlayerAppearance));
  std::vector<uint8_t> canonical;
  ASSERT_TRUE(adapter.validate_profile_extension(extension, canonical));
  ASSERT_EQ(canonical, extension);
  adapter.player_profile_changed({.player_id = 1,
                                  .display_name = "Daxter",
                                  .character = platform::PlayerCharacter::DAXTER,
                                  .game_extension = extension});
  adapter.tick(200);
  auto remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_TRUE(remote->identities[1].joined);
  EXPECT_EQ(remote->identities[1].name[0], 'D');
  adapter.player_departed(1);
  adapter.tick(208);
  remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_FALSE(remote->identities[1].joined);
}
TEST(Jak2AdapterIntegration, RapidRejoinRetainsLifecycleAndStartsFreshPresentation) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  const platform::PlayerProfile profile = {
      .player_id = 1,
      .display_name = "Remote",
      .character = platform::PlayerCharacter::JAK,
      .game_extension = std::vector<uint8_t>(sizeof(jak2::core::PlayerAppearance))};
  adapter.player_profile_changed(profile);
  jak2::core::PlayerState player = {};
  player.player_id = 1;
  player.activity = jak2::core::PlayerActivity::IN_GAME;
  player.position = {100.0f, 200.0f, 300.0f};
  std::vector<uint8_t> body;
  ASSERT_TRUE(encode_player_packet(player, body));
  ASSERT_NE(adapter.packets()
                .receive({.origin = {.authenticated_player_id = 1},
                          .message_id = static_cast<uint8_t>(PacketType::PLAYER_STATE),
                          .sequence = 100,
                          .received_at_ms = 100,
                          .payload = body},
                         endpoint)
                .disposition,
            platform::PayloadDisposition::REJECT);
  adapter.tick(100);
  adapter.player_departed(1);
  adapter.player_profile_changed(profile);
  player.position = {400.0f, 500.0f, 600.0f};
  ASSERT_TRUE(encode_player_packet(player, body));
  ASSERT_NE(adapter.packets()
                .receive({.origin = {.authenticated_player_id = 1},
                          .message_id = static_cast<uint8_t>(PacketType::PLAYER_STATE),
                          .sequence = 1,
                          .received_at_ms = 108,
                          .payload = body},
                         endpoint)
                .disposition,
            platform::PayloadDisposition::REJECT);
  adapter.tick(108);
  auto frame = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(frame);
  EXPECT_TRUE(frame->identities[1].joined);
  EXPECT_EQ(frame->player_lifecycles[1], 1u);
  EXPECT_EQ(frame->players[1].last_sequence, 1u);
  EXPECT_TRUE(frame->player_targets[1].valid);
  EXPECT_EQ(frame->player_targets[1].position, frame->players[1].position);
  EXPECT_EQ(frame->player_targets[1].generation, 1u);

  std::vector<jak2::core::GameEvent> events(3);
  events[0].source_player_id = 1;
  events[1].source_player_id = 2;
  events[2].source_player_id = 1;
  ASSERT_TRUE(adapter.mailbox().push_inbound_events(events));
  adapter.player_departed(1);
  const auto pending = adapter.mailbox().take_inbound_events(64);
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].source_player_id, 2u);
}

TEST(Jak2AdapterIntegration, GungameStateIsPeriodicAndRejectedPublicationDoesNotApplyLocally) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  endpoint.schedule_gungame = true;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  const auto publish = [&](uint32_t run, uint64_t time) {
    auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
    frame->local_player_id = 0;
    frame->host_player_id = 0;
    if (run != 0) {
      frame->gungame = jak2::core::GungameState{
          .run_id = run, .course_id = 3, .phase = jak2::core::GungamePhase::COURSE};
      frame->gungame->targets.resize(209, {.state = jak2::core::GungameTargetState::SPAWNED});
    }
    adapter.mailbox().publish_local_frame(std::move(frame));
    adapter.tick(time);
  };
  const auto count = [&] {
    return std::ranges::count_if(endpoint.sent, [](const auto& packet) {
      return packet.id == static_cast<uint8_t>(PacketType::GUNGAME_STATE);
    });
  };
  publish(1, 1000);
  EXPECT_EQ(count(), 1);
  auto remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->gungame.targets.size(), 209u);
  publish(2, 1249);
  EXPECT_EQ(count(), 1);
  endpoint.accept_sends = false;
  publish(2, 1250);
  EXPECT_EQ(count(), 1);
  remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->gungame.run_id, 1u);
  endpoint.accept_sends = true;
  publish(2, 1500);
  EXPECT_EQ(count(), 2);
  remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->gungame.run_id, 2u);
  publish(0, 1750);
  remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->gungame.phase, jak2::core::GungamePhase::INACTIVE);
  EXPECT_TRUE(remote->gungame.targets.empty());
  adapter.session_reset();
  adapter.tick(2000);
  remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->gungame.sequence, 0u);
}

TEST(Jak2AdapterIntegration, MissingGungameCaptureKeepsPlayerAndTrafficPublishing) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  endpoint.session_snapshot.state.status = platform::SessionStatus::IN_GAME;
  endpoint.session_snapshot.players = {{.player_id = 0}, {.player_id = 1}};
  endpoint.session_snapshot.connections = {{.player_id = 1, .network = {.connection_id = 11}}};
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  const auto publish = [&](bool capture_gungame, float position, uint64_t time) {
    auto frame = std::make_unique<jak2::application::LocalReplicationFrame>();
    frame->local_player_id = 0;
    frame->host_player_id = 0;
    frame->players[0].state_ready = true;
    frame->players[0].position[0] = position;
    frame->traffic_authority.revision = 1;
    frame->traffic_authority.assignments.fill(0);
    frame->selected_traffic_authority = 0;
    frame->vehicles.kind = jak2::core::TrafficSnapshot::Kind::VEHICLES;
    frame->vehicles.vehicles.push_back({.net_id = 1, .position = {position, 0.0f, 0.0f}});
    if (!capture_gungame)
      frame->gungame.reset();
    adapter.mailbox().publish_local_frame(std::move(frame));
    adapter.tick(time);
  };
  publish(false, 10.0f, 1000);
  publish(false, 20.0f, 1250);
  EXPECT_EQ(std::ranges::count_if(endpoint.sent,
                                  [](const auto& packet) {
                                    return packet.id ==
                                           static_cast<uint8_t>(PacketType::PLAYER_STATE);
                                  }),
            2);
  EXPECT_EQ(std::ranges::count_if(endpoint.sent,
                                  [](const auto& packet) {
                                    return packet.id ==
                                           static_cast<uint8_t>(PacketType::VEHICLE_STATE_BATCH);
                                  }),
            2);
  EXPECT_EQ(std::ranges::count_if(endpoint.sent,
                                  [](const auto& packet) {
                                    return packet.id ==
                                           static_cast<uint8_t>(PacketType::GUNGAME_STATE);
                                  }),
            0);
  auto remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_FLOAT_EQ(remote->players[0].position[0], 20.0f);
  EXPECT_EQ(remote->gungame.sequence, 0u);
  publish(true, 30.0f, 1500);
  EXPECT_EQ(std::ranges::count_if(endpoint.sent,
                                  [](const auto& packet) {
                                    return packet.id ==
                                           static_cast<uint8_t>(PacketType::GUNGAME_STATE);
                                  }),
            1);
}

TEST(Jak2AdapterIntegration, GungameReceivesCompleteHostStateAndRejectsStaleSequences) {
  jak2::application::Jak2Adapter adapter;
  RecordingEndpoint endpoint;
  endpoint.session_snapshot.state.role = platform::SessionRole::CLIENT;
  endpoint.session_snapshot.state.local_player_id = 1;
  adapter.installed(endpoint);
  adapter.session_started(endpoint.session_snapshot.state);
  jak2::core::GungameState state = {.run_id = 8,
                                    .score = 2300,
                                    .elapsed_time = 600,
                                    .course_id = 3,
                                    .phase = jak2::core::GungamePhase::COURSE};
  state.targets.resize(209, {.state = jak2::core::GungameTargetState::SPAWNED});
  state.targets.back() = {.state = jak2::core::GungameTargetState::BROKEN};
  const auto receive = [&](uint32_t sequence, bool host = true) {
    const auto bytes = platform::wire::encode_packet(jak2::wire::to_packet(state));
    EXPECT_TRUE(bytes);
    return adapter.packets()
        .receive({.origin = {.authenticated_player_id = static_cast<uint8_t>(host ? 0 : 2),
                             .from_host = host},
                  .message_id = static_cast<uint8_t>(PacketType::GUNGAME_STATE),
                  .sequence = sequence,
                  .payload = *bytes},
                 endpoint)
        .disposition;
  };
  EXPECT_EQ(receive(1, false), platform::PayloadDisposition::REJECT);
  EXPECT_EQ(receive(0), platform::PayloadDisposition::REJECT);
  EXPECT_EQ(receive(UINT32_MAX - 1), platform::PayloadDisposition::CONSUME);
  adapter.tick(1000);
  auto remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->gungame.targets, state.targets);
  EXPECT_EQ(remote->gungame.score, 2300);
  EXPECT_EQ(receive(UINT32_MAX - 1), platform::PayloadDisposition::REJECT);
  state.run_id = 9;
  state.targets.assign(209, {.state = jak2::core::GungameTargetState::NOT_SPAWNED});
  EXPECT_EQ(receive(1), platform::PayloadDisposition::CONSUME);
  EXPECT_EQ(receive(UINT32_MAX), platform::PayloadDisposition::REJECT);
  adapter.tick(1010);
  remote = adapter.mailbox().take_remote_frame();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->gungame.run_id, 9u);
  EXPECT_EQ(remote->gungame.targets.back().state, jak2::core::GungameTargetState::NOT_SPAWNED);
  EXPECT_TRUE(endpoint.sent.empty());
}

TEST(Jak2AdapterIntegration, GungameHitRequestsAndConfirmationsPreserveOriginsAndFullIds) {
  jak2::application::Jak2Adapter host, client;
  RecordingEndpoint host_endpoint, client_endpoint;
  client_endpoint.session_snapshot.state.role = platform::SessionRole::CLIENT;
  client_endpoint.session_snapshot.state.local_player_id = 1;
  host.installed(host_endpoint);
  client.installed(client_endpoint);
  host.session_started(host_endpoint.session_snapshot.state);
  client.session_started(client_endpoint.session_snapshot.state);
  jak2::core::GameEvent first = {.event_id = 44,
                                 .source_player_id = 0,
                                 .payload_size = 8,
                                 .payload = {9, 0, 0, 0, 0xec, 0x0e, 2, 1}};
  auto second = first;
  second.payload[4] = 0xf6;
  second.payload[7] = 0;
  ASSERT_TRUE(client.mailbox().push_outbound_events({first, second}));
  client_endpoint.accept_sends = false;
  client.tick(1000);
  EXPECT_EQ(client.mailbox().outbound_event_count(), 2u);
  EXPECT_TRUE(client_endpoint.sent.empty());
  client_endpoint.accept_sends = true;
  client.tick(1001);
  ASSERT_EQ(client_endpoint.sent.size(), 1u);
  EXPECT_EQ(client.mailbox().outbound_event_count(), 0u);
  const auto requests =
      host.packets().receive({.origin = {.authenticated_player_id = 1},
                              .message_id = static_cast<uint8_t>(PacketType::GAME_EVENT_BATCH),
                              .sequence = 1,
                              .payload = client_endpoint.sent.front().payload},
                             host_endpoint);
  EXPECT_EQ(requests.disposition, platform::PayloadDisposition::CONSUME_AND_RELAY);
  host.tick(1001);
  const auto accepted = host.mailbox().take_inbound_events(64);
  ASSERT_EQ(accepted.size(), 2u);
  EXPECT_EQ(accepted[0].payload, first.payload);
  EXPECT_EQ(accepted[1].payload, second.payload);
  EXPECT_EQ(accepted[0].source_player_id, 1u);
  EXPECT_EQ(accepted[1].source_player_id, 1u);
  // The GOAL manager queues confirmations after validating the run and applying a break.
  ASSERT_TRUE(host.mailbox().push_outbound_events(accepted));
  host.tick(1002);
  ASSERT_EQ(host_endpoint.sent.size(), 1u);
  EXPECT_EQ(client.packets()
                .receive({.origin = {.authenticated_player_id = 0, .from_host = true},
                          .message_id = static_cast<uint8_t>(PacketType::GAME_EVENT_BATCH),
                          .sequence = 2,
                          .payload = host_endpoint.sent.front().payload},
                         client_endpoint)
                .disposition,
            platform::PayloadDisposition::CONSUME);
  client.tick(1003);
  const auto confirmed = client.mailbox().take_inbound_events(64);
  ASSERT_EQ(confirmed.size(), 2u);
  EXPECT_EQ(confirmed[0].payload, first.payload);
  EXPECT_EQ(confirmed[1].payload, second.payload);
  EXPECT_EQ(confirmed[0].source_player_id, 0u);
  EXPECT_EQ(confirmed[1].source_player_id, 0u);
}

}  // namespace

TEST(Jak2AdapterIntegration, RegistryContainsEveryGameplayIdWithoutSharedControls) {
  jak2::application::Jak2Adapter adapter;
  ASSERT_EQ(adapter.packets().policies().size(), static_cast<uint8_t>(PacketType::COUNT));
  for (uint8_t id = 0; id < static_cast<uint8_t>(PacketType::COUNT); ++id) {
    const auto* handler = adapter.packets().find(id);
    ASSERT_NE(handler, nullptr);
    EXPECT_STRNE(handler->policy().name, "EVENT_JOIN");
    EXPECT_STRNE(handler->policy().name, "EVENT_LEAVE");
    EXPECT_STRNE(handler->policy().name, "LOBBY_ACTION");
    EXPECT_STRNE(handler->policy().name, "BOOTSTRAP");
  }
  RecordingEndpoint endpoint;
  EXPECT_EQ(adapter.packets().receive({.message_id = 255}, endpoint).disposition,
            platform::PayloadDisposition::REJECT);
}
