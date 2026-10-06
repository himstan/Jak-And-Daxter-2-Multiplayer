#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <vector>

#include "common/cross_sockets/XSocket.h"

#include "game/multiplayer/platform/runtime/multiplayer_runtime.h"
#include "game/multiplayer/platform/session/packet_registry.h"
#include "game/multiplayer/platform/session/session_controller.h"
#ifdef ENABLE_NETWORK_SIMULATION
#include "game/multiplayer/platform/transport/network_simulation.h"
#endif
#include "game/multiplayer/platform/transport/session_platform.h"
#include "gtest/gtest.h"
#include "steam/isteamnetworkingutils.h"
#include "steam/steamnetworkingsockets.h"

namespace {
using namespace multiplayer::platform;

class GnsDiagnosticsEnvironment final : public testing::Environment {
 public:
  void SetUp() override { initial_errors_ = transport_error_count(); }
  void TearDown() override {
    EXPECT_EQ(transport_error_count(), initial_errors_)
        << "GameNetworkingSockets emitted an error diagnostic";
  }

 private:
  uint64_t initial_errors_ = 0;
};

[[maybe_unused]] testing::Environment* const kGnsDiagnosticsEnvironment =
    testing::AddGlobalTestEnvironment(new GnsDiagnosticsEnvironment);

#ifdef ENABLE_NETWORK_SIMULATION
TEST(GnsTransportIntegration, NetworkSimulationRejectsSettingsOutsideGnsLimits) {
  NetworkSimulationSettings settings;
  EXPECT_TRUE(valid_network_simulation_settings(settings));
  settings.send_lag_ms = 5001;
  EXPECT_FALSE(valid_network_simulation_settings(settings));
  settings = {};
  settings.receive_loss_percent = 100.1f;
  EXPECT_FALSE(valid_network_simulation_settings(settings));
  settings = {};
  settings.send_rate_limit_burst_bytes = 1024 * 1024 + 1;
  EXPECT_FALSE(valid_network_simulation_settings(settings));
}
#endif

uint16_t available_udp_port() {
  const int socket = open_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket < 0)
    return 0;
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    close_socket(socket);
    return 0;
  }
  socklen_t size = sizeof(address);
  const bool found = getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0;
  close_socket(socket);
  return found ? ntohs(address.sin_port) : 0;
}

uint64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

template <typename Pump, typename Predicate>
bool pump_until(Pump pump,
                Predicate predicate,
                const std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    pump();
    if (predicate())
      return true;
    std::this_thread::yield();
  }
  return false;
}

TEST(GnsTransportIntegration, ClosedConnectionsRejectLaneQueriesWithoutDiagnostics) {
  SessionPlatform platform;
  auto* sockets = SteamNetworkingSockets();
  ASSERT_NE(sockets, nullptr);
  HSteamNetConnection first = k_HSteamNetConnection_Invalid;
  HSteamNetConnection second = k_HSteamNetConnection_Invalid;
  ASSERT_TRUE(sockets->CreateSocketPair(&first, &second, false, nullptr, nullptr));
  const int priorities[5] = {0, 0, 1, 1, 2};
  const uint16 weights[5] = {1, 4, 1, 1, 1};
  ASSERT_EQ(sockets->ConfigureConnectionLanes(second, 5, priorities, weights), k_EResultOK);
  ASSERT_TRUE(sockets->CloseConnection(first, 2005, "closed lane query test", false));
  ASSERT_TRUE(pump_until([] {},
                         [&] {
                           SteamNetConnectionInfo_t info = {};
                           return sockets->GetConnectionInfo(second, &info) &&
                                  info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer;
                         }));
  SteamNetConnectionRealTimeStatus_t status = {};
  SteamNetConnectionRealTimeLaneStatus_t lanes[5] = {};
  EXPECT_EQ(sockets->GetConnectionRealTimeStatus(second, &status, 5, lanes), k_EResultNoConnection);
  EXPECT_TRUE(sockets->CloseConnection(second, 2005, "lane query test complete", false));
}

class GnsSimulationScope {
 public:
  GnsSimulationScope() {
    const char* configured = std::getenv("GNS_TEST_SEED");
    seed_ = configured ? static_cast<unsigned int>(std::strtoul(configured, nullptr, 10)) : 1337;
    std::srand(seed_);
  }
  ~GnsSimulationScope() {
    SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send,
                                                      0.0f);
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send,
                                                      0);
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(
        k_ESteamNetworkingConfig_FakeRateLimit_Send_Rate, 0);
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(
        k_ESteamNetworkingConfig_FakeRateLimit_Send_Burst, 16 * 1024);
  }
  void loss(float percent) {
    ASSERT_TRUE(SteamNetworkingUtils()->SetGlobalConfigValueFloat(
        k_ESteamNetworkingConfig_FakePacketLoss_Send, percent))
        << "GNS_TEST_SEED=" << seed_;
  }
  void rate_limit(const int32_t bytes_per_second, const int32_t burst_bytes) {
    ASSERT_TRUE(SteamNetworkingUtils()->SetGlobalConfigValueInt32(
        k_ESteamNetworkingConfig_FakeRateLimit_Send_Rate, bytes_per_second));
    ASSERT_TRUE(SteamNetworkingUtils()->SetGlobalConfigValueInt32(
        k_ESteamNetworkingConfig_FakeRateLimit_Send_Burst, burst_bytes));
  }
  unsigned int seed() const { return seed_; }

 private:
  unsigned int seed_ = 1337;
};

struct ReceivedGameplay {
  PlayerId origin = kInvalidPlayerId;
  uint8_t id = 0;
  uint32_t sequence = 0;
  uint64_t received_at_ms = 0;
  std::vector<uint8_t> payload;
};

constexpr GameMessagePolicy kTestPolicy = {
    .id = 0,
    .name = "STATE",
    .direction = MessageDirection::BIDIRECTIONAL,
    .delivery = Delivery::UNRELIABLE_REALTIME,
    .cadence = CadenceMode::ON_DEMAND,
    .maximum_payload_bytes = 64 * 1024,
};

class RecordingHandler final : public PacketHandler {
 public:
  explicit RecordingHandler(std::function<ValidatedPayload(const GameplayMessage&)> record)
      : PacketHandler(kTestPolicy), record_(std::move(record)) {}
  ValidatedPayload receive(const GameplayMessage& message, GameSessionEndpoint&) override {
    return record_(message);
  }
  void publish(GameSessionEndpoint&, uint64_t) override {}

 private:
  std::function<ValidatedPayload(const GameplayMessage&)> record_;
};

std::vector<std::unique_ptr<PacketHandler>> recording_handlers(
    std::function<ValidatedPayload(const GameplayMessage&)> record) {
  std::vector<std::unique_ptr<PacketHandler>> handlers;
  handlers.push_back(std::make_unique<RecordingHandler>(std::move(record)));
  return handlers;
}

class RecordingAdapter final : public GameAdapter {
 public:
  explicit RecordingAdapter(std::string game_id = "test", std::string identity = "test-build")
      : packets_(recording_handlers([this](const auto& message) { return record(message); }),
                 64 * 1024) {
    descriptor_.game_id = std::move(game_id);
    descriptor_.compatibility_identity = std::move(identity);
    descriptor_.maximum_payload_bytes = 64 * 1024;
    descriptor_.maximum_players = 8;
    descriptor_.maximum_profile_extension_bytes = 16;
    descriptor_.supported_characters = {PlayerCharacter::JAK, PlayerCharacter::DAXTER};
  }

  const GameDescriptor& descriptor() const override { return descriptor_; }
  void session_started(const SessionState&) override { ++session_start_count; }
  void session_reset() override { ++session_reset_count; }
  void tick(uint64_t) override {}
  void stop() override {}
  bool validate_profile_extension(std::span<const uint8_t> extension,
                                  std::vector<uint8_t>& canonical) override {
    canonical.assign(extension.begin(), extension.end());
    return true;
  }
  ValidatedPayload record(const GameplayMessage& message) {
    if (disconnect_payloads)
      return {.disposition = PayloadDisposition::DISCONNECT};
    received.push_back(
        {.origin = message.origin.authenticated_player_id,
         .id = message.message_id,
         .sequence = message.sequence,
         .received_at_ms = message.received_at_ms,
         .payload = std::vector<uint8_t>(message.payload.begin(), message.payload.end())});
    ValidatedPayload result = {
        .disposition = relay ? PayloadDisposition::CONSUME_AND_RELAY : PayloadDisposition::CONSUME,
        .canonical_payload = received.back().payload};
    result.relay_recipients = relay_recipients;
    return result;
  }
  std::vector<uint8_t> create_bootstrap(PlayerId player_id) override { return {player_id, 0x42}; }
  bool apply_bootstrap(uint32_t generation, std::span<const uint8_t> payload) override {
    ++bootstrap_apply_attempts;
    if (!apply_succeeds)
      return false;
    applied_generation = generation;
    applied_bootstrap.assign(payload.begin(), payload.end());
    return true;
  }
  void player_profile_changed(const PlayerProfile& value) override {
    profile_changes.push_back(value);
  }
  void player_departed(PlayerId player_id) override { departures.push_back(player_id); }

  PacketRegistry& packets() override { return packets_; }
  PacketRegistry packets_;
  GameDescriptor descriptor_;
  bool relay = false;
  bool disconnect_payloads = false;
  std::optional<std::vector<PlayerId>> relay_recipients;
  bool apply_succeeds = true;
  uint32_t bootstrap_apply_attempts = 0;
  uint32_t applied_generation = 0;
  uint32_t session_start_count = 0;
  uint32_t session_reset_count = 0;
  std::vector<uint8_t> applied_bootstrap;
  std::vector<ReceivedGameplay> received;
  std::vector<PlayerProfile> profile_changes;
  std::vector<PlayerId> departures;
};

struct RuntimeAdapterState {
  std::atomic_uint32_t ticks = 0;
  std::atomic_uint32_t starts = 0;
  std::atomic_uint32_t resets = 0;
  std::atomic_uint32_t bootstraps = 0;
  std::atomic_bool stopped = false;
  std::function<void()> on_bootstrap;
};

class RuntimeAdapter final : public GameAdapter {
 public:
  explicit RuntimeAdapter(std::shared_ptr<RuntimeAdapterState> state) : state_(std::move(state)) {}
  PacketRegistry& packets() override { return packets_; }
  const GameDescriptor& descriptor() const override { return descriptor_; }
  bool configure_compatibility_identity(std::string identity) override {
    if (identity.empty())
      return false;
    descriptor_.compatibility_identity = std::move(identity);
    return true;
  }
  void session_started(const SessionState&) override { ++state_->starts; }
  void session_reset() override { ++state_->resets; }
  void tick(uint64_t) override { ++state_->ticks; }
  void stop() override { state_->stopped = true; }
  bool validate_profile_extension(std::span<const uint8_t> extension,
                                  std::vector<uint8_t>& canonical) override {
    canonical.assign(extension.begin(), extension.end());
    return true;
  }
  bool apply_bootstrap(uint32_t, std::span<const uint8_t>) override {
    ++state_->bootstraps;
    if (state_->on_bootstrap)
      state_->on_bootstrap();
    return true;
  }

 private:
  std::shared_ptr<RuntimeAdapterState> state_;
  GameDescriptor descriptor_ = {
      .game_id = "test",
      .compatibility_identity = "test-build",
      .maximum_payload_bytes = 64 * 1024,
      .maximum_players = 8,
      .maximum_profile_extension_bytes = 16,
      .supported_characters = {PlayerCharacter::JAK, PlayerCharacter::DAXTER}};
  PacketRegistry packets_{recording_handlers([](const auto&) { return ValidatedPayload{}; }),
                          64 * 1024};
};

PlayerProfile profile(std::string name = "Player") {
  return {.display_name = std::move(name), .character = PlayerCharacter::JAK};
}

ControllerHostConfig host_config(uint16_t port, uint8_t limit = 8) {
  return {
      .port = port, .player_limit = limit, .room_code = "ABC123", .local_profile = profile("Host")};
}

ControllerClientConfig client_config(uint16_t port) {
  return {.endpoint = "127.0.0.1",
          .port = port,
          .room_code = "ABC123",
          .local_profile = profile("Client")};
}

struct RuntimeCommandProbe {
  std::atomic_bool entered = false;
  std::atomic_bool release = false;
  std::mutex mutex;
  std::vector<int> values;
  std::vector<uint32_t> tick_samples;
};

class BlockingControlCommand final : public RuntimeControlCommand {
 public:
  explicit BlockingControlCommand(std::shared_ptr<RuntimeCommandProbe> probe)
      : probe_(std::move(probe)) {}
  uint8_t action() const final { return 240; }

 private:
  CommandError execute(RuntimeControlExecutionPort&) final {
    probe_->entered = true;
    while (!probe_->release.load())
      std::this_thread::yield();
    return CommandError::NONE;
  }
  std::shared_ptr<RuntimeCommandProbe> probe_;
};

class RecordingControlCommand final : public RuntimeControlCommand {
 public:
  RecordingControlCommand(std::shared_ptr<RuntimeCommandProbe> probe,
                          std::shared_ptr<RuntimeAdapterState> adapter,
                          const int value)
      : probe_(std::move(probe)), adapter_(std::move(adapter)), value_(value) {}
  uint8_t action() const final { return 241; }

 private:
  CommandError execute(RuntimeControlExecutionPort&) final {
    std::lock_guard lock(probe_->mutex);
    probe_->values.push_back(value_);
    probe_->tick_samples.push_back(adapter_->ticks.load());
    return CommandError::NONE;
  }
  std::shared_ptr<RuntimeCommandProbe> probe_;
  std::shared_ptr<RuntimeAdapterState> adapter_;
  int value_ = 0;
};

class ThrowingControlCommand final : public RuntimeControlCommand {
 public:
  uint8_t action() const final { return 242; }

 private:
  CommandError execute(RuntimeControlExecutionPort&) final {
    throw std::runtime_error("expected test failure");
  }
};

TEST(GnsTransportIntegration, CapacityRejectionsDoNotPreventLaterConnections) {
  SessionPlatform host;
  ASSERT_TRUE(host.host({.port = available_udp_port(), .maximum_pending_connections = 1}));
  SessionPlatform first;
  ASSERT_TRUE(first.connect({.endpoint = "127.0.0.1", .port = host.local_port()}));
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        first.pump();
      },
      [&] { return host.connection_snapshots().size() == 1; }));
  for (size_t attempt = 0; attempt < 3; ++attempt) {
    SessionPlatform rejected;
    ASSERT_TRUE(rejected.connect({.endpoint = "127.0.0.1", .port = host.local_port()}));
    ASSERT_TRUE(pump_until(
        [&] {
          host.pump();
          first.pump();
          rejected.pump();
        },
        [&] {
          TransportEvent event;
          while (rejected.poll_event(event)) {
            if (event.kind == TransportEventKind::CLOSED)
              return true;
          }
          return false;
        }));
    EXPECT_EQ(host.connection_snapshots().size(), 1u);
  }
  first.shutdown();
  ASSERT_TRUE(
      pump_until([&] { host.pump(); }, [&] { return host.connection_snapshots().empty(); }));
  SessionPlatform replacement;
  ASSERT_TRUE(replacement.connect({.endpoint = "127.0.0.1", .port = host.local_port()}));
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        replacement.pump();
      },
      [&] {
        return host.connection_snapshots().size() == 1 &&
               replacement.connection_snapshots().size() == 1;
      }));
}

TEST(GnsTransportIntegration, RawTransportConfiguresLanesAndDeliversLargeMessagesWhole) {
  SessionPlatform host;
  SessionPlatform client;
  ASSERT_TRUE(host.host({.port = available_udp_port()}));
  ASSERT_TRUE(client.connect({.endpoint = "127.0.0.1", .port = host.local_port()}));
  ConnectionId host_connection_id = 0;
  ConnectionId client_connection = 0;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        client.pump();
      },
      [&] {
        TransportEvent event;
        while (host.poll_event(event))
          if (event.kind == TransportEventKind::CONNECTED)
            host_connection_id = event.connection_id;
        while (client.poll_event(event))
          if (event.kind == TransportEventKind::CONNECTED)
            client_connection = event.connection_id;
        return host_connection_id != 0 && client_connection != 0;
      }));
  std::vector<uint8_t> large(32 * 1024, 0xa5);
  ASSERT_TRUE(client.send(client_connection, large, Delivery::RELIABLE_ORDERED));
  TransportEvent received;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        client.pump();
      },
      [&] {
        TransportEvent event;
        while (host.poll_event(event)) {
          if (event.kind == TransportEventKind::MESSAGE) {
            received = std::move(event);
            return true;
          }
        }
        return false;
      }));
  EXPECT_EQ(received.connection_id, host_connection_id);
  EXPECT_EQ(received.delivery, Delivery::RELIABLE_ORDERED);
  EXPECT_EQ(received.payload, large);
  const auto connections = host.connection_snapshots();
  ASSERT_EQ(connections.size(), 1);
  const auto aggregate = host.aggregate_snapshot();
  EXPECT_GE(aggregate.pending_unreliable_bytes, 0);
  EXPECT_GE(aggregate.pending_reliable_bytes, 0);
  EXPECT_GE(aggregate.sent_unacked_reliable_bytes, 0);
  host.shutdown();
  client.shutdown();
  EXPECT_FALSE(host.active());
  EXPECT_FALSE(client.active());
}

TEST(GnsTransportIntegration, RawTransportPreservesOrderAcrossReceivePumpBudget) {
  SessionPlatform host;
  SessionPlatform client;
  ASSERT_TRUE(host.host({.port = available_udp_port()}));
  ASSERT_TRUE(client.connect({.endpoint = "127.0.0.1", .port = host.local_port()}));
  ConnectionId client_connection = 0;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        client.pump();
      },
      [&] {
        TransportEvent event;
        while (host.poll_event(event)) {
        }
        while (client.poll_event(event)) {
          if (event.kind == TransportEventKind::CONNECTED)
            client_connection = event.connection_id;
        }
        return client_connection != 0;
      }));

  constexpr size_t kMessageCount = kMaximumTransportMessagesPerPump + 64;
  for (size_t index = 0; index < kMessageCount; ++index) {
    const std::array<uint8_t, 2> payload = {static_cast<uint8_t>(index),
                                            static_cast<uint8_t>(index >> 8)};
    ASSERT_TRUE(client.send(client_connection, payload, Delivery::RELIABLE_ORDERED));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  host.pump();
  std::vector<uint16_t> received;
  TransportEvent event;
  while (host.poll_event(event)) {
    if (event.kind == TransportEventKind::MESSAGE) {
      ASSERT_EQ(event.payload.size(), 2u);
      received.push_back(static_cast<uint16_t>(event.payload[0]) |
                         static_cast<uint16_t>(event.payload[1] << 8));
    }
  }
  ASSERT_EQ(received.size(), kMaximumTransportMessagesPerPump);

  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        client.pump();
      },
      [&] {
        while (host.poll_event(event)) {
          if (event.kind == TransportEventKind::MESSAGE) {
            if (event.payload.size() != 2)
              return false;
            received.push_back(static_cast<uint16_t>(event.payload[0]) |
                               static_cast<uint16_t>(event.payload[1] << 8));
          }
        }
        return received.size() == kMessageCount;
      }));
  ASSERT_EQ(received.size(), kMessageCount);
  for (size_t index = 0; index < received.size(); ++index)
    EXPECT_EQ(received[index], index);
  host.shutdown();
  client.shutdown();
}

TEST(GnsTransportIntegration, CriticalRealtimePreemptsAConstrainedBulkBacklog) {
  SessionPlatform host;
  SessionPlatform client;
  GnsSimulationScope simulation;
  ASSERT_TRUE(host.host({.port = available_udp_port()}));
  ASSERT_TRUE(client.connect({.endpoint = "127.0.0.1", .port = host.local_port()}));
  ConnectionId client_connection = 0;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        client.pump();
      },
      [&] {
        const auto snapshots = client.connection_snapshots();
        if (!snapshots.empty())
          client_connection = snapshots.front().connection_id;
        return client_connection != 0 && !host.connection_snapshots().empty();
      }));

  simulation.rate_limit(32 * 1024, 1200);
  constexpr size_t kBulkMessageCount = 24;
  const std::vector<uint8_t> bulk(5 * 1024, 0xb0);
  for (size_t index = 0; index < kBulkMessageCount; ++index)
    ASSERT_TRUE(client.send(client_connection, bulk, TransportLane::REALTIME_BULK));
  ASSERT_TRUE(client.send(client_connection, std::array<uint8_t, 1>{0xc0},
                          TransportLane::REALTIME_CRITICAL));

  size_t bulk_before_critical = 0;
  bool critical_received = false;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        client.pump();
      },
      [&] {
        TransportEvent event;
        while (host.poll_event(event)) {
          if (event.kind != TransportEventKind::MESSAGE)
            continue;
          if (event.lane == TransportLane::REALTIME_CRITICAL) {
            critical_received = true;
            return true;
          }
          if (event.lane == TransportLane::REALTIME_BULK)
            ++bulk_before_critical;
        }
        return false;
      },
      std::chrono::seconds(2)));
  EXPECT_TRUE(critical_received);
  EXPECT_LT(bulk_before_critical, kBulkMessageCount);
  simulation.rate_limit(0, 16 * 1024);
  client.shutdown();
  host.shutdown();
}

TEST(GnsTransportIntegration, ReliableLanePreservesMessageOrder) {
  SessionPlatform host;
  SessionPlatform client;
  ASSERT_TRUE(host.host({.port = available_udp_port()}));
  ASSERT_TRUE(client.connect({.endpoint = "127.0.0.1", .port = host.local_port()}));
  ConnectionId client_connection = 0;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        client.pump();
      },
      [&] {
        TransportEvent event;
        while (client.poll_event(event)) {
          if (event.kind == TransportEventKind::CONNECTED)
            client_connection = event.connection_id;
        }
        return client_connection != 0 && !host.connection_snapshots().empty();
      }));
  for (uint8_t value = 0; value < 64; ++value) {
    ASSERT_TRUE(
        client.send(client_connection, std::array<uint8_t, 1>{value}, Delivery::RELIABLE_ORDERED));
  }
  std::vector<uint8_t> received;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        client.pump();
      },
      [&] {
        TransportEvent event;
        while (host.poll_event(event)) {
          if (event.kind == TransportEventKind::MESSAGE)
            received.push_back(event.payload.front());
        }
        return received.size() == 64;
      }));
  for (size_t index = 0; index < received.size(); ++index)
    EXPECT_EQ(received[index], index);
}

TEST(GnsTransportIntegration, LossKeepsReliableOrderAndDoesNotReplayRealtimeBacklog) {
  SessionPlatform host;
  SessionPlatform client;
  GnsSimulationScope simulation;
  ASSERT_TRUE(host.host({.port = available_udp_port()}));
  ASSERT_TRUE(client.connect({.endpoint = "127.0.0.1", .port = host.local_port()}));
  ConnectionId client_connection = 0;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        client.pump();
      },
      [&] {
        const auto snapshots = client.connection_snapshots();
        if (!snapshots.empty())
          client_connection = snapshots.front().connection_id;
        return client_connection != 0 && !host.connection_snapshots().empty();
      }))
      << "GNS_TEST_SEED=" << simulation.seed();

  simulation.loss(35.0f);
  for (uint8_t value = 0; value < 32; ++value) {
    ASSERT_TRUE(
        client.send(client_connection, std::array<uint8_t, 1>{value}, Delivery::RELIABLE_ORDERED));
  }
  std::vector<uint8_t> reliable;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        client.pump();
      },
      [&] {
        TransportEvent event;
        while (host.poll_event(event)) {
          if (event.kind == TransportEventKind::MESSAGE)
            reliable.push_back(event.payload.front());
        }
        return reliable.size() == 32;
      },
      std::chrono::seconds(10)))
      << "GNS_TEST_SEED=" << simulation.seed();
  for (size_t index = 0; index < reliable.size(); ++index)
    EXPECT_EQ(reliable[index], index);

  simulation.loss(100.0f);
  for (uint8_t value = 0; value < 100; ++value) {
    ASSERT_TRUE(client.send(client_connection, std::array<uint8_t, 1>{value},
                            Delivery::UNRELIABLE_REALTIME));
    client.pump();
  }
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        client.pump();
      },
      [&] {
        const auto snapshots = client.connection_snapshots();
        return !snapshots.empty() && snapshots.front().pending_unreliable_bytes == 0;
      }))
      << "GNS_TEST_SEED=" << simulation.seed();
  simulation.loss(0.0f);
  ASSERT_TRUE(
      client.send(client_connection, std::array<uint8_t, 1>{0xff}, Delivery::UNRELIABLE_REALTIME));
  bool latest_received = false;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        client.pump();
      },
      [&] {
        TransportEvent event;
        while (host.poll_event(event)) {
          if (event.kind == TransportEventKind::MESSAGE && event.payload.front() == 0xff) {
            latest_received = true;
          }
        }
        return latest_received;
      }))
      << "GNS_TEST_SEED=" << simulation.seed();
}

TEST(GnsTransportIntegration, ReliableBacklogSaturationClosesOnlyTheAffectedPeer) {
  SessionPlatform host;
  SessionPlatform saturated_client;
  SessionPlatform healthy_client;
  GnsSimulationScope simulation;
  ASSERT_TRUE(host.host({.port = available_udp_port()}));
  ASSERT_TRUE(saturated_client.connect({.endpoint = "127.0.0.1", .port = host.local_port()}));
  ASSERT_TRUE(healthy_client.connect({.endpoint = "127.0.0.1", .port = host.local_port()}));
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        saturated_client.pump();
        healthy_client.pump();
      },
      [&] {
        return host.connection_snapshots().size() == 2 &&
               !saturated_client.connection_snapshots().empty() &&
               !healthy_client.connection_snapshots().empty();
      }))
      << "GNS_TEST_SEED=" << simulation.seed();
  ASSERT_TRUE(saturated_client.send(saturated_client.host_connection_id(),
                                    std::array<uint8_t, 1>{0xa1}, Delivery::RELIABLE_ORDERED));
  ASSERT_TRUE(healthy_client.send(healthy_client.host_connection_id(), std::array<uint8_t, 1>{0xb2},
                                  Delivery::RELIABLE_ORDERED));
  ConnectionId saturated_connection = 0;
  ConnectionId healthy_connection = 0;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        saturated_client.pump();
        healthy_client.pump();
      },
      [&] {
        TransportEvent event;
        while (host.poll_event(event)) {
          if (event.kind != TransportEventKind::MESSAGE || event.payload.size() != 1)
            continue;
          if (event.payload.front() == 0xa1)
            saturated_connection = event.connection_id;
          if (event.payload.front() == 0xb2)
            healthy_connection = event.connection_id;
        }
        return saturated_connection != 0 && healthy_connection != 0;
      }))
      << "GNS_TEST_SEED=" << simulation.seed();
  ASSERT_NE(saturated_connection, healthy_connection);

  simulation.loss(100.0f);
  const std::vector<uint8_t> block(64 * 1024, 0x5a);
  for (int index = 0; index < 7; ++index) {
    ASSERT_TRUE(host.send(saturated_connection, block, Delivery::RELIABLE_ORDERED));
  }
  ConnectionId closed_connection = 0;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        saturated_client.pump();
        healthy_client.pump();
      },
      [&] {
        TransportEvent event;
        while (host.poll_event(event)) {
          if (event.kind == TransportEventKind::CLOSED)
            closed_connection = event.connection_id;
        }
        return closed_connection != 0;
      }))
      << "GNS_TEST_SEED=" << simulation.seed();
  EXPECT_EQ(closed_connection, saturated_connection);
  const auto survivors = host.connection_snapshots();
  ASSERT_EQ(survivors.size(), 1u);
  EXPECT_EQ(survivors.front().connection_id, healthy_connection);

  simulation.loss(0.0f);
  ASSERT_TRUE(
      host.send(healthy_connection, std::array<uint8_t, 1>{0x7e}, Delivery::RELIABLE_ORDERED));
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump();
        healthy_client.pump();
      },
      [&] {
        TransportEvent event;
        while (healthy_client.poll_event(event)) {
          if (event.kind == TransportEventKind::MESSAGE && event.payload.front() == 0x7e) {
            return true;
          }
        }
        return false;
      }))
      << "GNS_TEST_SEED=" << simulation.seed();
}

TEST(GnsTransportIntegration, RuntimeOwnsAdapterWorkerAndShutsDownIdempotently) {
  auto state = std::make_shared<RuntimeAdapterState>();
  MultiplayerRuntime runtime;
  ASSERT_TRUE(runtime.install(std::make_unique<RuntimeAdapter>(state)));
  EXPECT_FALSE(runtime.install(std::make_unique<RuntimeAdapter>(state)));
  ASSERT_TRUE(runtime.enqueue<SetCompatibilityIdentityCommand>("updated-build"));
  ASSERT_TRUE(pump_until([] {},
                         [&] {
                           const auto snapshot = runtime.snapshot();
                           return snapshot.control_result.outcome == CommandOutcome::APPLIED &&
                                  snapshot.compatibility_identity == "updated-build";
                         }));
  EXPECT_EQ(runtime.snapshot().compatibility_identity, "updated-build");
  ASSERT_TRUE(pump_until([] {}, [&] { return state->ticks.load() != 0; }));
  ASSERT_TRUE(runtime.enqueue<ConnectSessionCommand>(ControllerClientConfig{
      .endpoint = "", .port = 0, .room_code = "bad", .local_profile = profile()}));
  ASSERT_TRUE(pump_until(
      [] {},
      [&] { return runtime.snapshot().connection_result.outcome == CommandOutcome::REJECTED; }));
  EXPECT_EQ(runtime.snapshot().connection_result.error, CommandError::INVALID_REQUEST);
  runtime.shutdown();
  EXPECT_TRUE(state->stopped.load());
  EXPECT_FALSE(runtime.active());
  runtime.shutdown();
}

TEST(GnsTransportIntegration, RuntimeCanReinstallAfterImmediateShutdown) {
  MultiplayerRuntime runtime;
  auto first = std::make_shared<RuntimeAdapterState>();
  ASSERT_TRUE(runtime.install(std::make_unique<RuntimeAdapter>(first)));
  runtime.shutdown();
  EXPECT_TRUE(first->stopped.load());
  EXPECT_FALSE(runtime.active());
  EXPECT_EQ(runtime.adapter("test"), nullptr);

  auto second = std::make_shared<RuntimeAdapterState>();
  ASSERT_TRUE(runtime.install(std::make_unique<RuntimeAdapter>(second)));
  ASSERT_TRUE(pump_until([] {}, [&] { return second->ticks.load() != 0; }));
  EXPECT_TRUE(runtime.active());
  runtime.shutdown();
  EXPECT_TRUE(second->stopped.load());
}

TEST(GnsTransportIntegration, RuntimeDestructorStopsAndJoinsWorker) {
  auto state = std::make_shared<RuntimeAdapterState>();
  {
    MultiplayerRuntime runtime;
    ASSERT_TRUE(runtime.install(std::make_unique<RuntimeAdapter>(state)));
    ASSERT_TRUE(pump_until([] {}, [&] { return state->ticks.load() != 0; }));
  }
  EXPECT_TRUE(state->stopped.load());
}

TEST(GnsTransportIntegration, RuntimeRejectsInactiveAndFullMailboxesWithoutLosingNewerResult) {
  MultiplayerRuntime runtime;
  EXPECT_FALSE(runtime.enqueue<StopDiscoveryCommand>());
  EXPECT_EQ(runtime.snapshot().connection_result.outcome, CommandOutcome::REJECTED);
  EXPECT_EQ(runtime.snapshot().connection_result.error, CommandError::RUNTIME_INACTIVE);

  auto adapter = std::make_shared<RuntimeAdapterState>();
  auto probe = std::make_shared<RuntimeCommandProbe>();
  ASSERT_TRUE(runtime.install(std::make_unique<RuntimeAdapter>(adapter)));
  ASSERT_TRUE(runtime.enqueue<BlockingControlCommand>(probe));
  ASSERT_TRUE(pump_until([] {}, [&] { return probe->entered.load(); }));
  for (size_t index = 0; index < 256; ++index)
    ASSERT_TRUE(runtime.enqueue<StopDiscoveryCommand>()) << index;
  EXPECT_FALSE(runtime.enqueue<StopDiscoveryCommand>());
  EXPECT_EQ(runtime.snapshot().connection_result.outcome, CommandOutcome::REJECTED);
  EXPECT_EQ(runtime.snapshot().connection_result.error, CommandError::QUEUE_FULL);
  probe->release = true;
  ASSERT_TRUE(pump_until([] {}, [&] { return adapter->ticks.load() != 0; }));
  EXPECT_EQ(runtime.snapshot().connection_result.outcome, CommandOutcome::REJECTED);
  EXPECT_EQ(runtime.snapshot().connection_result.error, CommandError::QUEUE_FULL);
  runtime.shutdown();
}

TEST(GnsTransportIntegration, RuntimeMailboxIsFifoAndPumpsAfterThirtyTwoCommands) {
  auto adapter = std::make_shared<RuntimeAdapterState>();
  auto probe = std::make_shared<RuntimeCommandProbe>();
  MultiplayerRuntime runtime;
  ASSERT_TRUE(runtime.install(std::make_unique<RuntimeAdapter>(adapter)));
  ASSERT_TRUE(runtime.enqueue<BlockingControlCommand>(probe));
  ASSERT_TRUE(pump_until([] {}, [&] { return probe->entered.load(); }));
  for (int value = 0; value < 64; ++value)
    ASSERT_TRUE(runtime.enqueue<RecordingControlCommand>(probe, adapter, value));
  probe->release = true;
  ASSERT_TRUE(pump_until([] {},
                         [&] {
                           std::lock_guard lock(probe->mutex);
                           return probe->values.size() == 64;
                         }));
  std::lock_guard lock(probe->mutex);
  for (int value = 0; value < 64; ++value)
    EXPECT_EQ(probe->values[value], value);
  EXPECT_GT(probe->tick_samples[32], probe->tick_samples[31]);
  runtime.shutdown();
}

TEST(GnsTransportIntegration, RuntimePublishesQueuedCompletionAndContainsExceptions) {
  auto adapter = std::make_shared<RuntimeAdapterState>();
  auto probe = std::make_shared<RuntimeCommandProbe>();
  MultiplayerRuntime runtime;
  ASSERT_TRUE(runtime.install(std::make_unique<RuntimeAdapter>(adapter)));
  ASSERT_TRUE(runtime.enqueue<BlockingControlCommand>(probe));
  ASSERT_TRUE(pump_until([] {}, [&] { return probe->entered.load(); }));
  EXPECT_EQ(runtime.snapshot().control_result.outcome, CommandOutcome::QUEUED);
  probe->release = true;
  ASSERT_TRUE(pump_until(
      [] {}, [&] { return runtime.snapshot().control_result.outcome == CommandOutcome::APPLIED; }));

  ASSERT_TRUE(runtime.enqueue<ThrowingControlCommand>());
  ASSERT_TRUE(pump_until([] {},
                         [&] {
                           const auto result = runtime.snapshot().control_result;
                           return result.outcome == CommandOutcome::REJECTED &&
                                  result.error == CommandError::INTERNAL_ERROR;
                         }));
  ASSERT_TRUE(runtime.enqueue<RecordingControlCommand>(probe, adapter, 7));
  ASSERT_TRUE(pump_until([] {},
                         [&] {
                           std::lock_guard lock(probe->mutex);
                           return !probe->values.empty();
                         }));
  EXPECT_TRUE(runtime.active());
  runtime.shutdown();
}

TEST(GnsTransportIntegration, CompatibilityIdentityRequiresIdleSessionAndDiscovery) {
  auto state = std::make_shared<RuntimeAdapterState>();
  MultiplayerRuntime runtime;
  ASSERT_TRUE(runtime.install(std::make_unique<RuntimeAdapter>(state)));
  ASSERT_TRUE(runtime.enqueue<StartDiscoveryCommand>(
      DiscoveryRequest{.discovery_port = available_udp_port(), .profile = profile()}));
  ASSERT_TRUE(pump_until(
      [] {}, [&] { return runtime.snapshot().discovery.status == DiscoveryStatus::SEARCHING; }));
  ASSERT_TRUE(runtime.enqueue<SetCompatibilityIdentityCommand>("forbidden-build"));
  ASSERT_TRUE(pump_until(
      [] {},
      [&] { return runtime.snapshot().control_result.outcome == CommandOutcome::REJECTED; }));
  EXPECT_EQ(runtime.snapshot().control_result.error, CommandError::NOT_ALLOWED);
  EXPECT_EQ(runtime.snapshot().compatibility_identity, "test-build");
  runtime.shutdown();
}

TEST(GnsTransportIntegration, RuntimeHostWaitsForTheGameToChooseLobbyOrInGame) {
  auto state = std::make_shared<RuntimeAdapterState>();
  MultiplayerRuntime runtime;
  const ProfileStorageConfig storage = {
      .game_id = "test",
      .root_directory =
          fs::temp_directory_path() / ("jadmp-host-character-" + std::to_string(now_ms()))};
  ASSERT_TRUE(runtime.install(std::make_unique<RuntimeAdapter>(state), storage));
  auto config = host_config(available_udp_port(), 2);
  config.local_profile.character = PlayerCharacter::DAXTER;
  ASSERT_TRUE(runtime.enqueue<HostSessionCommand>(HostSessionRequest{.config = config}));
  ASSERT_TRUE(pump_until(
      [] {}, [&] { return runtime.snapshot().session.state.status == SessionStatus::CONNECTING; }));
  EXPECT_EQ(runtime.snapshot().session.state.role, SessionRole::HOST);
  EXPECT_EQ(runtime.snapshot().session.players.front().character, PlayerCharacter::DAXTER);
  StoredPlayerProfile stored;
  ASSERT_TRUE(runtime.load_profile(stored));
  EXPECT_EQ(stored.preferred_character, PlayerCharacter::JAK);

  ASSERT_TRUE(runtime.enqueue<EnterLobbyCommand>());
  ASSERT_TRUE(pump_until(
      [] {}, [&] { return runtime.snapshot().session.state.status == SessionStatus::LOBBY; }));

  ASSERT_TRUE(runtime.enqueue<StartCountdownCommand>(3));
  ASSERT_TRUE(pump_until([] {}, [&] { return runtime.snapshot().session.countdown_active; }));
  ASSERT_TRUE(runtime.load_profile(stored));
  EXPECT_EQ(stored.preferred_character, PlayerCharacter::DAXTER);

  ASSERT_TRUE(runtime.enqueue<StartGameCommand>());
  ASSERT_TRUE(pump_until(
      [] {},
      [&] { return runtime.snapshot().session.state.status == SessionStatus::GAME_STARTING; }));

  ASSERT_TRUE(runtime.enqueue<EnterGameCommand>());
  ASSERT_TRUE(pump_until(
      [] {}, [&] { return runtime.snapshot().session.state.status == SessionStatus::IN_GAME; }));
  runtime.shutdown();
}

TEST(GnsTransportIntegration, LobbyCharacterPersistsOnlyWhenReadyAndLocksUntilUnready) {
  RecordingAdapter host_adapter;
  SessionController host(host_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 2)));
  host.enter_lobby();

  auto state = std::make_shared<RuntimeAdapterState>();
  MultiplayerRuntime client;
  const ProfileStorageConfig storage = {
      .game_id = "test",
      .root_directory =
          fs::temp_directory_path() / ("jadmp-client-character-" + std::to_string(now_ms()))};
  ASSERT_TRUE(client.install(std::make_unique<RuntimeAdapter>(state), storage));
  ASSERT_TRUE(client.save_profile({.display_name = "Client"}));
  ASSERT_TRUE(client.enqueue<ConnectSessionCommand>(client_config(host.local_port())));
  auto pump = [&] { host.pump(now_ms()); };
  ASSERT_TRUE(pump_until(pump, [&] { return host.snapshot().players.size() == 2; }));

  StoredPlayerProfile stored;
  ASSERT_TRUE(client.enqueue<SetCharacterCommand>(PlayerCharacter::DAXTER));
  ASSERT_TRUE(pump_until(
      pump, [&] { return host.snapshot().players.back().character == PlayerCharacter::DAXTER; }));
  ASSERT_TRUE(client.load_profile(stored));
  EXPECT_EQ(stored.preferred_character, PlayerCharacter::JAK);

  ASSERT_TRUE(client.enqueue<SetReadyCommand>(true));
  ASSERT_TRUE(pump_until(pump, [&] { return host.snapshot().players.back().ready; }));
  ASSERT_TRUE(client.load_profile(stored));
  EXPECT_EQ(stored.preferred_character, PlayerCharacter::DAXTER);
  ASSERT_TRUE(client.enqueue<SetCharacterCommand>(PlayerCharacter::JAK));
  ASSERT_TRUE(pump_until(
      pump, [&] { return client.snapshot().session_result.outcome == CommandOutcome::REJECTED; }));
  EXPECT_EQ(client.snapshot().session_result.error, CommandError::INVALID_STATE);
  EXPECT_EQ(host.snapshot().players.back().character, PlayerCharacter::DAXTER);

  const auto connection = host.snapshot().connections.front().network.connection_id;
  host.transport().close_connection(connection, 4002, "reconnect ready character");
  ASSERT_TRUE(pump_until(
      pump, [&] { return client.snapshot().session.state.status == SessionStatus::RECONNECTING; }));
  ASSERT_TRUE(pump_until(pump, [&] {
    return client.snapshot().session.state.status == SessionStatus::LOBBY &&
           host.snapshot().players.size() == 2;
  }));
  EXPECT_EQ(host.snapshot().players.back().character, PlayerCharacter::DAXTER);

  ASSERT_TRUE(client.enqueue<SetReadyCommand>(false));
  ASSERT_TRUE(pump_until(pump, [&] { return !host.snapshot().players.back().ready; }));
  ASSERT_TRUE(client.enqueue<SetCharacterCommand>(PlayerCharacter::JAK));
  ASSERT_TRUE(pump_until(
      pump, [&] { return host.snapshot().players.back().character == PlayerCharacter::JAK; }));
  ASSERT_TRUE(client.load_profile(stored));
  EXPECT_EQ(stored.preferred_character, PlayerCharacter::DAXTER);
  ASSERT_TRUE(client.enqueue<SetReadyCommand>(true));
  ASSERT_TRUE(pump_until(pump, [&] { return host.snapshot().players.back().ready; }));
  ASSERT_TRUE(client.load_profile(stored));
  EXPECT_EQ(stored.preferred_character, PlayerCharacter::JAK);
  client.shutdown();
  auto lease = ProfileLease::acquire(storage);
  ASSERT_TRUE(lease);
  ASSERT_TRUE(load_player_profile(*lease, stored));
  EXPECT_EQ(stored.preferred_character, PlayerCharacter::JAK);
  EXPECT_EQ(stored.display_name, "Client");
}

TEST(GnsTransportIntegration, AbruptLossReconnectsAndLateJoinReceivesBootstrap) {
  RecordingAdapter host_adapter;
  SessionController host(host_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 2)));
  host.enter_lobby();
  ASSERT_TRUE(host.start_game());
  EXPECT_EQ(host.snapshot().state.status, SessionStatus::GAME_STARTING);
  host.enter_game();

  auto state = std::make_shared<RuntimeAdapterState>();
  MultiplayerRuntime client;
  state->on_bootstrap = [&] { client.enqueue<EnterGameCommand>(); };
  ASSERT_TRUE(client.install(std::make_unique<RuntimeAdapter>(state)));
  auto request = client_config(host.local_port());
  request.local_profile.character = PlayerCharacter::DAXTER;
  ASSERT_TRUE(client.enqueue<ConnectSessionCommand>(request));
  const bool joined_in_game =
      pump_until([&] { host.pump(now_ms()); },
                 [&] {
                   return client.snapshot().session.state.status == SessionStatus::IN_GAME &&
                          host.snapshot().players.size() == 2;
                 });
  ASSERT_TRUE(joined_in_game) << "status="
                              << static_cast<int>(client.snapshot().session.state.status)
                              << " players=" << host.snapshot().players.size()
                              << " starts=" << state->starts.load()
                              << " bootstraps=" << state->bootstraps.load();
  EXPECT_EQ(state->starts.load(), 1u);
  EXPECT_EQ(state->bootstraps.load(), 1u);
  EXPECT_EQ(host.snapshot().players.back().character, PlayerCharacter::DAXTER);

  const auto connections = host.transport().connection_snapshots();
  ASSERT_EQ(connections.size(), 1u);
  host.transport().close_connection(connections.front().connection_id, 4002,
                                    "simulated abrupt loss");
  ASSERT_TRUE(pump_until(
      [&] { host.pump(now_ms()); },
      [&] { return client.snapshot().session.state.status == SessionStatus::RECONNECTING; }));
  const bool reconnected =
      pump_until([&] { host.pump(now_ms()); },
                 [&] {
                   return client.snapshot().session.state.status == SessionStatus::IN_GAME &&
                          host.snapshot().players.size() == 2 && state->starts.load() == 2;
                 });
  ASSERT_TRUE(reconnected) << "status=" << static_cast<int>(client.snapshot().session.state.status)
                           << " error="
                           << static_cast<int>(client.snapshot().connection_result.error)
                           << " players=" << host.snapshot().players.size()
                           << " starts=" << state->starts.load()
                           << " resets=" << state->resets.load()
                           << " bootstraps=" << state->bootstraps.load();
  EXPECT_GE(state->resets.load(), 1u);
  EXPECT_EQ(state->bootstraps.load(), 2u);
  EXPECT_EQ(host.snapshot().players.back().character, PlayerCharacter::DAXTER);
  ASSERT_EQ(host.snapshot().connections.size(), 1u);
  EXPECT_EQ(host.snapshot().connections.front().player_id, 1u);
  ASSERT_EQ(client.snapshot().session.connections.size(), 1u);
  EXPECT_EQ(client.snapshot().session.connections.front().player_id, 0u);

  const auto connections2 = host.transport().connection_snapshots();
  ASSERT_EQ(connections2.size(), 1u);
  host.transport().close_connection(connections2.front().connection_id, 4002,
                                    "simulated second abrupt loss");
  ASSERT_TRUE(pump_until(
      [&] { host.pump(now_ms()); },
      [&] { return client.snapshot().session.state.status == SessionStatus::RECONNECTING; }));
  const bool reconnected2 =
      pump_until([&] { host.pump(now_ms()); },
                 [&] {
                   return client.snapshot().session.state.status == SessionStatus::IN_GAME &&
                          host.snapshot().players.size() == 2 && state->starts.load() == 3;
                 });
  ASSERT_TRUE(reconnected2) << "status=" << static_cast<int>(client.snapshot().session.state.status)
                            << " error="
                            << static_cast<int>(client.snapshot().connection_result.error)
                            << " players=" << host.snapshot().players.size()
                            << " starts=" << state->starts.load()
                            << " resets=" << state->resets.load()
                            << " bootstraps=" << state->bootstraps.load();
  EXPECT_GE(state->resets.load(), 2u);
  EXPECT_EQ(state->bootstraps.load(), 3u);
  EXPECT_EQ(host.snapshot().players.back().character, PlayerCharacter::DAXTER);
  ASSERT_EQ(host.snapshot().connections.size(), 1u);
  EXPECT_EQ(host.snapshot().connections.front().player_id, 1u);
  ASSERT_EQ(client.snapshot().session.connections.size(), 1u);
  EXPECT_EQ(client.snapshot().session.connections.front().player_id, 0u);
  client.shutdown();
}

TEST(GnsTransportIntegration, SessionAdmissionProfilesAndRelayStayAboveTransport) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  RecordingAdapter observer_adapter;
  host_adapter.relay = true;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  SessionController observer(observer_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port())));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(observer.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        client.pump(now);
        observer.pump(now);
      },
      [&] {
        return client.snapshot().state.status == SessionStatus::LOBBY &&
               observer.snapshot().state.status == SessionStatus::LOBBY &&
               host.snapshot().players.size() == 3;
      }));
  ASSERT_EQ(host.snapshot().connections.size(), 2u);
  EXPECT_EQ(host.snapshot().connections[0].player_id, 1u);
  EXPECT_EQ(host.snapshot().connections[1].player_id, 2u);
  EXPECT_NE(host.snapshot().connections[0].network.connection_id, 0u);
  EXPECT_NE(host.snapshot().connections[1].network.connection_id, 0u);
  ASSERT_EQ(client.snapshot().connections.size(), 1u);
  EXPECT_EQ(client.snapshot().connections.front().player_id, 0u);
  EXPECT_EQ(client.snapshot().connections.front().network.connection_id,
            client.transport().host_connection_id());
  const std::vector<uint8_t> payload(4096, 0x5a);
  const auto first_sequence = client.send_gameplay(0, Audience::everyone(), payload);
  ASSERT_TRUE(first_sequence);
  EXPECT_EQ(*first_sequence, 1u);
  uint64_t host_receive_pump_ms = 0;
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        const auto host_received_before = host_adapter.received.size();
        host.pump(now);
        if (host_adapter.received.size() != host_received_before) {
          host_receive_pump_ms = now;
        }
        client.pump(now);
        observer.pump(now);
      },
      [&] { return host_adapter.received.size() == 1 && observer_adapter.received.size() == 1; }));
  EXPECT_TRUE(client_adapter.received.empty());
  EXPECT_EQ(host_adapter.received[0].origin, client.snapshot().state.local_player_id);
  EXPECT_EQ(host_adapter.received[0].received_at_ms, host_receive_pump_ms);
  EXPECT_EQ(observer_adapter.received[0].payload, payload);
  EXPECT_EQ(observer_adapter.profile_changes.size(), 3u);

  host_adapter.relay_recipients.emplace();
  ASSERT_TRUE(client.send_gameplay(0, Audience::everyone(), std::array<uint8_t, 1>{0x61}));
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        client.pump(now);
        observer.pump(now);
      },
      [&] { return host_adapter.received.size() == 2; }));
  for (size_t iteration = 0; iteration < 20; ++iteration) {
    const auto now = now_ms();
    host.pump(now);
    client.pump(now);
    observer.pump(now);
  }
  EXPECT_EQ(observer_adapter.received.size(), 1u);

  const auto observer_id = observer.snapshot().state.local_player_id;
  ASSERT_NE(observer_id, kInvalidPlayerId);
  ASSERT_NE(observer_id, client.snapshot().state.local_player_id);
  host_adapter.relay_recipients = std::vector<PlayerId>{observer_id};
  ASSERT_TRUE(client.send_gameplay(0, Audience::everyone(), std::array<uint8_t, 1>{0x62}));
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        client.pump(now);
        observer.pump(now);
      },
      [&] { return host_adapter.received.size() == 3 && observer_adapter.received.size() == 2; }));
  EXPECT_EQ(observer_adapter.received.back().payload, std::vector<uint8_t>({0x62}));
}

TEST(GnsTransportIntegration, HostSharesPlayerPingsWithEveryClientAndClearsDepartedPlayers) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  RecordingAdapter observer_adapter;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  SessionController observer(observer_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 3)));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(observer.connect(client_config(host.local_port())));
  auto clock = now_ms();
  const auto pump = [&] {
    host.pump(clock);
    client.pump(clock);
    observer.pump(clock);
  };
  ASSERT_TRUE(pump_until(pump, [&] {
    return host.snapshot().players.size() == 3 && client.snapshot().players.size() == 3 &&
           observer.snapshot().players.size() == 3;
  }));
  clock += 1000;
  pump();
  const auto published = host.snapshot().player_pings;
  ASSERT_EQ(published.size(), 3u);
  EXPECT_EQ(published[0], 0);
  EXPECT_NE(published[1], kUnknownPlayerPing);
  EXPECT_NE(published[2], kUnknownPlayerPing);
  ASSERT_TRUE(pump_until(pump, [&] {
    return client.snapshot().player_pings == published &&
           observer.snapshot().player_pings == published;
  }));
  for (PlayerId id = 0; id < 3; ++id) {
    EXPECT_EQ(client.snapshot().player_ping_ms(id), published[id]);
    EXPECT_EQ(observer.snapshot().player_ping_ms(id), published[id]);
  }
  const auto departed = client.snapshot().state.local_player_id;
  client.disconnect();
  ASSERT_TRUE(pump_until(pump, [&] {
    return host.snapshot().players.size() == 2 && observer.snapshot().players.size() == 2;
  }));
  EXPECT_FALSE(host.snapshot().player_ping_ms(departed));
  EXPECT_FALSE(observer.snapshot().player_ping_ms(departed));
  host.disconnect();
  ASSERT_TRUE(pump_until(pump, [&] {
    return observer.snapshot().state.status == SessionStatus::HOST_LEFT;
  }));
  EXPECT_TRUE(observer.snapshot().player_pings.empty());
}

TEST(GnsTransportIntegration, DuplicateControlsAreSuppressedAndFloodClosesOnlyTheSender) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  RecordingAdapter observer_adapter;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  SessionController observer(observer_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 3)));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(observer.connect(client_config(host.local_port())));
  auto clock = now_ms();
  const auto pump = [&] {
    host.pump(clock);
    client.pump(clock);
    observer.pump(clock);
  };
  ASSERT_TRUE(pump_until(pump, [&] {
    return host.snapshot().players.size() == 3 && client.snapshot().players.size() == 3 &&
           observer.snapshot().players.size() == 3;
  }));
  const auto player_id = client.snapshot().state.local_player_id;
  auto duplicate = profile("Client");
  duplicate.player_id = player_id;
  const auto send = [&](const ControlMessage& message) {
    const auto payload = encode_control_message(message, 16);
    const auto frame = encode_message_frame(FrameKind::CONTROL, player_id, payload);
    return client.transport().send(client.transport().host_connection_id(), frame,
                                   TransportLane::CONTROL_RELIABLE);
  };
  host_adapter.profile_changes.clear();
  ASSERT_TRUE(send({.kind = ControlKind::PROFILE, .profile = duplicate}));
  ASSERT_TRUE(send({.kind = ControlKind::SET_READY, .ready = false}));
  ASSERT_TRUE(send({.kind = ControlKind::SET_CHARACTER, .character = PlayerCharacter::JAK}));
  ASSERT_TRUE(send({.kind = ControlKind::SET_READY, .ready = true}));
  ASSERT_TRUE(pump_until(pump, [&] {
    return std::ranges::any_of(observer.snapshot().players, [&](const auto& player) {
      return player.player_id == player_id && player.ready;
    });
  }));
  ASSERT_EQ(host_adapter.profile_changes.size(), 1u);
  EXPECT_TRUE(host_adapter.profile_changes.front().ready);

  for (size_t iteration = 0; iteration < 2; ++iteration) {
    clock += 4000;
    for (size_t update = 0; update < 20; ++update)
      ASSERT_TRUE(send({.kind = ControlKind::SET_READY, .ready = update % 2 != 0}));
    ASSERT_TRUE(pump_until(
        pump, [&] { return host_adapter.profile_changes.size() == 1 + (iteration + 1) * 20; }));
    ASSERT_EQ(host.snapshot().connections.size(), 2u);
  }
  for (size_t update = 0; update < 40; ++update)
    ASSERT_TRUE(send({.kind = ControlKind::SET_READY, .ready = true}));
  ASSERT_TRUE(pump_until(pump, [&] { return host.snapshot().connections.size() == 1; }));
  EXPECT_EQ(host.snapshot().connections.front().player_id,
            observer.snapshot().state.local_player_id);
  EXPECT_EQ(observer.snapshot().state.status, SessionStatus::LOBBY);
  ASSERT_TRUE(observer.set_ready(true));
  ASSERT_TRUE(pump_until(pump, [&] {
    return host_adapter.profile_changes.back().player_id ==
           observer.snapshot().state.local_player_id;
  }));

  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(pump, [&] { return host.snapshot().players.size() == 3; }));
  EXPECT_EQ(client.snapshot().state.local_player_id, player_id);
}

TEST(GnsTransportIntegration, AdapterCapacityFailureClosesTheSendingConnection) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  host_adapter.disconnect_payloads = true;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 2)));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  const auto pump = [&] {
    const auto now = now_ms();
    host.pump(now);
    client.pump(now);
  };
  ASSERT_TRUE(pump_until(pump, [&] { return host.snapshot().players.size() == 2; }));
  ASSERT_TRUE(client.send_gameplay(0, Audience::everyone(), std::array<uint8_t, 1>{7}));
  ASSERT_TRUE(pump_until(pump, [&] { return host.snapshot().connections.empty(); }));
  EXPECT_TRUE(host_adapter.received.empty());
  ASSERT_EQ(host_adapter.departures.size(), 1u);
}

TEST(GnsTransportIntegration, GameplaySequencesRejectInvalidSendsWorkWithoutPeersAndReset) {
  RecordingAdapter adapter;
  SessionController session(adapter);
  const std::array<uint8_t, 1> payload = {0x42};
  EXPECT_FALSE(session.send_gameplay(0, Audience::everyone(), payload));
  ASSERT_TRUE(session.host(host_config(available_udp_port())));
  const auto first = session.send_gameplay(0, Audience::everyone(), payload);
  const auto second = session.send_gameplay(0, Audience::everyone(), payload);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(*first, 1u);
  EXPECT_EQ(*second, 2u);
  EXPECT_FALSE(session.send_gameplay(0, Audience::one(UINT32_MAX), payload));
  const auto after_rejection = session.send_gameplay(0, Audience::everyone(), payload);
  ASSERT_TRUE(after_rejection);
  EXPECT_EQ(*after_rejection, 4u);
  EXPECT_FALSE(session.send_gameplay(1, Audience::everyone(), payload));
  const std::vector<uint8_t> oversized(64 * 1024 + 1);
  EXPECT_FALSE(session.send_gameplay(0, Audience::everyone(), oversized));
  session.disconnect();
  ASSERT_TRUE(session.host(host_config(available_udp_port())));
  const auto reset = session.send_gameplay(0, Audience::everyone(), payload);
  ASSERT_TRUE(reset);
  EXPECT_EQ(*reset, 1u);
}

TEST(GnsTransportIntegration, GameplayBroadcastRejectsAClosedRecipientBeforeRosterRemoval) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port())));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump(now_ms());
        client.pump(now_ms());
      },
      [&] {
        return client.snapshot().state.status == SessionStatus::LOBBY &&
               host.snapshot().players.size() == 2;
      }));
  const auto connections = host.transport().connection_snapshots();
  ASSERT_EQ(connections.size(), 1u);
  const auto connection = connections.front().connection_id;
  host.transport().close_connection(connection, 2005, "test closed gameplay recipient");
  ASSERT_EQ(host.snapshot().players.size(), 2u);
  const std::array<uint8_t, 1> payload = {0x42};
  EXPECT_FALSE(host.send_gameplay(0, Audience::everyone(), payload));
  EXPECT_FALSE(host.send_gameplay(0, Audience::one(connection), payload));
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump(now_ms());
        client.pump(now_ms());
      },
      [&] { return host.snapshot().players.size() == 1; }));
  const auto local_only = host.send_gameplay(0, Audience::everyone(), payload);
  ASSERT_TRUE(local_only);
  EXPECT_EQ(*local_only, 3u);
}

TEST(GnsTransportIntegration, ClientSubmissionRejectsAnUnavailableHostBeforeReconnectProcessing) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port())));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump(now_ms());
        client.pump(now_ms());
      },
      [&] {
        return client.snapshot().state.status == SessionStatus::LOBBY &&
               host.snapshot().players.size() == 2;
      }));
  const auto connection = client.transport().host_connection_id();
  client.transport().close_connection(connection, 2005, "test unavailable gameplay host");
  ASSERT_EQ(client.snapshot().state.status, SessionStatus::LOBBY);
  const std::array<uint8_t, 1> payload = {0x42};
  EXPECT_FALSE(client.send_gameplay(0, Audience::everyone(), payload));
  EXPECT_FALSE(client.send_gameplay(0, Audience::one(connection), payload));
  client.pump(now_ms());
  EXPECT_EQ(client.snapshot().state.status, SessionStatus::RECONNECTING);
}

TEST(GnsTransportIntegration, ZeroSequenceGameplayClosesTheSenderBeforeAdapterDispatch) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port())));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump(now_ms());
        client.pump(now_ms());
      },
      [&] {
        return client.snapshot().state.status == SessionStatus::LOBBY &&
               host.snapshot().players.size() == 2;
      }));
  const std::array<uint8_t, 1> payload = {0x42};
  auto envelope = encode_gameplay_envelope(0, 1, payload);
  std::fill(envelope.begin() + 1, envelope.begin() + 5, 0);
  const auto frame =
      encode_message_frame(FrameKind::GAMEPLAY, client.snapshot().state.local_player_id, envelope);
  ASSERT_TRUE(client.transport().send(client.transport().host_connection_id(), frame,
                                      TransportLane::REALTIME_NORMAL));
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump(now_ms());
        client.pump(now_ms());
      },
      [&] {
        return host.snapshot().players.size() == 1 &&
               client.snapshot().state.status == SessionStatus::RECONNECTING;
      }));
  EXPECT_TRUE(host_adapter.received.empty());
  EXPECT_TRUE(client_adapter.received.empty());
}

TEST(GnsTransportIntegration, GameplaySendsRejectWhileClientIsReconnecting) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port())));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        client.pump(now);
      },
      [&] { return client.snapshot().state.status == SessionStatus::LOBBY; }));
  const auto connections = host.transport().connection_snapshots();
  ASSERT_EQ(connections.size(), 1u);
  host.transport().close_connection(connections.front().connection_id, 4002,
                                    "test reconnecting send");
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        client.pump(now);
      },
      [&] { return client.snapshot().state.status == SessionStatus::RECONNECTING; }));
  EXPECT_FALSE(client.send_gameplay(0, Audience::everyone(), std::array<uint8_t, 1>{0x42}));
}

TEST(GnsTransportIntegration, SpoofedClientOriginClosesOnlyThatPlayer) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 2)));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        client.pump(now);
      },
      [&] { return host.snapshot().players.size() == 2; }));
  const auto gameplay = encode_gameplay_envelope(0, 1, std::array<uint8_t, 1>{0x44});
  const auto spoofed = encode_message_frame(FrameKind::GAMEPLAY, 0, gameplay);
  ASSERT_TRUE(client.transport().send(client.transport().host_connection_id(), spoofed,
                                      Delivery::UNRELIABLE_REALTIME));
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        client.pump(now);
      },
      [&] { return host.snapshot().players.size() == 1; }));
  EXPECT_TRUE(host_adapter.received.empty());
  ASSERT_EQ(host_adapter.departures.size(), 1u);
  EXPECT_EQ(host_adapter.departures.front(), 1u);
}

TEST(GnsTransportIntegration, SessionReportsTypedGateRejections) {
  struct Case {
    RejectionReason reason;
    std::unique_ptr<RecordingAdapter> adapter;
    std::string room;
  };
  std::vector<Case> cases;
  cases.push_back(
      {RejectionReason::WRONG_GAME, std::make_unique<RecordingAdapter>("other"), "ABC123"});
  cases.push_back({RejectionReason::VERSION_MISMATCH,
                   std::make_unique<RecordingAdapter>("test", "other-build"), "ABC123"});
  cases.push_back(
      {RejectionReason::WRONG_ROOM_CODE, std::make_unique<RecordingAdapter>(), "BAD999"});
  for (auto& test_case : cases) {
    RecordingAdapter host_adapter;
    SessionController host(host_adapter);
    SessionController client(*test_case.adapter);
    ASSERT_TRUE(host.host(host_config(available_udp_port())));
    auto config = client_config(host.local_port());
    config.room_code = test_case.room;
    ASSERT_TRUE(client.connect(config));
    ASSERT_TRUE(pump_until(
        [&] {
          const auto now = now_ms();
          host.pump(now);
          client.pump(now);
        },
        [&] { return client.snapshot().state.status == SessionStatus::FAILED; }));
    EXPECT_EQ(client.snapshot().rejection, test_case.reason);
  }
}

TEST(GnsTransportIntegration, HostCapacityAndPlayerSlotReuseAreSessionOwned) {
  RecordingAdapter host_adapter;
  RecordingAdapter first_adapter;
  RecordingAdapter rejected_adapter;
  SessionController host(host_adapter);
  SessionController first(first_adapter);
  SessionController rejected(rejected_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 2)));
  ASSERT_TRUE(first.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        first.pump(now);
      },
      [&] { return first.snapshot().state.status == SessionStatus::LOBBY; }));
  EXPECT_EQ(first.snapshot().state.local_player_id, 1);
  ASSERT_TRUE(rejected.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        first.pump(now);
        rejected.pump(now);
      },
      [&] { return rejected.snapshot().state.status == SessionStatus::FAILED; }));
  EXPECT_EQ(rejected.snapshot().rejection, RejectionReason::HOST_FULL);

  first.disconnect();
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
      },
      [&] {
        return host.snapshot().players.size() == 1 &&
               host.transport().connection_snapshots().empty() &&
               host.snapshot().connections.empty();
      }));
  RecordingAdapter replacement_adapter;
  SessionController replacement(replacement_adapter);
  ASSERT_TRUE(replacement.connect(client_config(host.local_port())));
  const bool replacement_joined = pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        replacement.pump(now);
      },
      [&] { return replacement.snapshot().state.status == SessionStatus::LOBBY; });
  ASSERT_TRUE(replacement_joined)
      << "replacement_status=" << static_cast<int>(replacement.snapshot().state.status)
      << " rejection=" << static_cast<int>(replacement.snapshot().rejection)
      << " host_players=" << host.snapshot().players.size()
      << " host_connections=" << host.transport().connection_snapshots().size()
      << " client_connections=" << replacement.transport().connection_snapshots().size();
  EXPECT_EQ(replacement.snapshot().state.local_player_id, 1);
  ASSERT_EQ(host.snapshot().connections.size(), 1u);
  EXPECT_EQ(host.snapshot().connections.front().player_id, 1u);
  ASSERT_EQ(replacement.snapshot().connections.size(), 1u);
  EXPECT_EQ(replacement.snapshot().connections.front().player_id, 0u);
}

TEST(GnsTransportIntegration, BootstrapRetriesUntilSuccessfulApplyAndAcknowledgement) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 2)));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        client.pump(now);
      },
      [&] { return host.snapshot().players.size() == 2; }));
  client_adapter.apply_succeeds = false;
  host.enter_lobby();
  ASSERT_TRUE(host.start_game());
  host.enter_game();
  const auto first = now_ms();
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump(first);
        client.pump(first);
      },
      [&] { return client_adapter.bootstrap_apply_attempts != 0; }));
  EXPECT_EQ(client_adapter.applied_generation, 0u);
  client_adapter.apply_succeeds = true;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump(first + 600);
        client.pump(first + 600);
      },
      [&] { return client_adapter.applied_generation != 0; }));
  EXPECT_EQ(client_adapter.applied_bootstrap,
            (std::vector<uint8_t>{client.snapshot().state.local_player_id, 0x42}));
}

TEST(GnsTransportIntegration, HostShutdownUsesTheTypedSessionClosePath) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 2)));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        client.pump(now);
      },
      [&] { return client.snapshot().state.status == SessionStatus::LOBBY; }));
  EXPECT_EQ(client_adapter.session_start_count, 1u);
  host.disconnect(23);
  const bool host_left =
      pump_until([&] { client.pump(now_ms()); },
                 [&] { return client.snapshot().state.status == SessionStatus::HOST_LEFT; });
  ASSERT_TRUE(host_left) << "client_status=" << static_cast<int>(client.snapshot().state.status)
                         << " close_reason=" << static_cast<int>(client.snapshot().close_reason)
                         << " connections=" << client.transport().connection_snapshots().size();
  EXPECT_EQ(client.snapshot().close_reason, 23u);
  EXPECT_EQ(client_adapter.session_reset_count, 1u);
}

TEST(GnsTransportIntegration, LobbyTransitionsRejectInvalidStatesAndReachLateJoins) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 2)));
  host.enter_lobby();
  EXPECT_FALSE(host.cancel_countdown());
  host.pump(now_ms());
  ASSERT_TRUE(host.start_countdown(5));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        client.pump(now);
      },
      [&] {
        return client.snapshot().state.status == SessionStatus::LOBBY &&
               client.snapshot().countdown_active;
      }));
  ASSERT_TRUE(host.start_game());
  EXPECT_EQ(host.snapshot().state.status, SessionStatus::GAME_STARTING);
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        client.pump(now);
      },
      [&] { return client.snapshot().state.status == SessionStatus::GAME_STARTING; }));
  EXPECT_FALSE(host.start_game());
  EXPECT_FALSE(host.start_countdown(1));
  EXPECT_FALSE(host.cancel_countdown());
  EXPECT_FALSE(host.set_ready(true));
  EXPECT_FALSE(host.set_character(PlayerCharacter::DAXTER));
}

TEST(GnsTransportIntegration, HostAdmitsSevenClientsAndRejectsTheEighth) {
  RecordingAdapter host_adapter;
  SessionController host(host_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 8)));
  std::vector<std::unique_ptr<RecordingAdapter>> adapters;
  std::vector<std::unique_ptr<SessionController>> clients;
  for (size_t index = 0; index < 7; ++index) {
    adapters.push_back(std::make_unique<RecordingAdapter>());
    clients.push_back(std::make_unique<SessionController>(*adapters.back()));
    ASSERT_TRUE(clients.back()->connect(client_config(host.local_port())));
  }
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        for (auto& client : clients)
          client->pump(now);
      },
      [&] {
        return host.snapshot().players.size() == 8 &&
               std::ranges::all_of(clients, [](const auto& client) {
                 return client->snapshot().state.status == SessionStatus::LOBBY;
               });
      }));
  std::vector<PlayerId> assigned;
  for (const auto& client : clients) {
    assigned.push_back(client->snapshot().state.local_player_id);
  }
  std::ranges::sort(assigned);
  EXPECT_EQ(assigned, (std::vector<PlayerId>{1, 2, 3, 4, 5, 6, 7}));

  RecordingAdapter overflow_adapter;
  SessionController overflow(overflow_adapter);
  ASSERT_TRUE(overflow.connect(client_config(host.local_port())));
  ASSERT_TRUE(pump_until(
      [&] {
        const auto now = now_ms();
        host.pump(now);
        overflow.pump(now);
      },
      [&] { return overflow.snapshot().state.status == SessionStatus::FAILED; }));
  EXPECT_EQ(overflow.snapshot().rejection, RejectionReason::HOST_FULL);
}

TEST(GnsTransportIntegration, PendingGateTimesOutWithTypedRejection) {
  RecordingAdapter host_adapter;
  SessionController host(host_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 2)));
  SessionPlatform raw_client;
  ASSERT_TRUE(raw_client.connect({.endpoint = "127.0.0.1", .port = host.local_port()}));
  const auto start = now_ms();
  ASSERT_TRUE(pump_until(
      [&] {
        raw_client.pump();
        // GNS callbacks are shared; drain host events before advancing its clock.
        host.pump(start);
      },
      [&] {
        return !raw_client.connection_snapshots().empty() &&
               !host.transport().connection_snapshots().empty();
      }));
  host.pump(start + 5001);
  ServerGate rejection;
  ASSERT_TRUE(pump_until(
      [&] {
        host.pump(start + 5001);
        raw_client.pump();
      },
      [&] {
        TransportEvent event;
        while (raw_client.poll_event(event)) {
          if (event.kind == TransportEventKind::MESSAGE &&
              decode_server_gate(event.payload, rejection))
            return true;
        }
        return false;
      }));
  EXPECT_FALSE(rejection.accepted);
  EXPECT_EQ(rejection.rejection, RejectionReason::GATE_TIMEOUT);
}

TEST(GnsTransportIntegration, MalformedGatesAndRepeatedRejectionsAreThrottled) {
  RecordingAdapter host_adapter;
  SessionController host(host_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 2)));
  {
    SessionPlatform raw_client;
    ASSERT_TRUE(raw_client.connect({.endpoint = "127.0.0.1", .port = host.local_port()}));
    ConnectionId connection = 0;
    ASSERT_TRUE(pump_until(
        [&] {
          const auto now = now_ms();
          host.pump(now);
          raw_client.pump();
        },
        [&] {
          TransportEvent event;
          while (raw_client.poll_event(event)) {
            if (event.kind == TransportEventKind::CONNECTED)
              connection = event.connection_id;
          }
          return connection != 0;
        }));
    ASSERT_TRUE(
        raw_client.send(connection, std::array<uint8_t, 1>{0xff}, Delivery::RELIABLE_ORDERED));
    ServerGate rejection;
    ASSERT_TRUE(pump_until(
        [&] {
          const auto now = now_ms();
          host.pump(now);
          raw_client.pump();
        },
        [&] {
          TransportEvent event;
          while (raw_client.poll_event(event)) {
            if (event.kind == TransportEventKind::MESSAGE &&
                decode_server_gate(event.payload, rejection))
              return true;
          }
          return false;
        }));
    EXPECT_EQ(rejection.rejection, RejectionReason::MALFORMED_GATE);
  }

  for (int attempt = 0; attempt < 6; ++attempt) {
    RecordingAdapter client_adapter;
    SessionController client(client_adapter);
    auto config = client_config(host.local_port());
    config.room_code = "BAD999";
    ASSERT_TRUE(client.connect(config));
    ASSERT_TRUE(pump_until(
        [&] {
          const auto now = now_ms();
          host.pump(now);
          client.pump(now);
        },
        [&] { return client.snapshot().state.status == SessionStatus::FAILED; }));
    if (attempt == 5)
      EXPECT_EQ(client.snapshot().rejection, RejectionReason::THROTTLED);
  }
}
}  // namespace

TEST(GnsTransportIntegration, AdapterPolicyAllowsLiveProfilesWithoutUnlockingReadyLobbyCharacters) {
  RecordingAdapter host_adapter;
  RecordingAdapter client_adapter;
  RecordingAdapter observer_adapter;
  SessionController host(host_adapter);
  SessionController client(client_adapter);
  SessionController observer(observer_adapter);
  ASSERT_TRUE(host.host(host_config(available_udp_port(), 3)));
  ASSERT_TRUE(client.connect(client_config(host.local_port())));
  ASSERT_TRUE(observer.connect(client_config(host.local_port())));
  const auto pump = [&] {
    const auto now = now_ms();
    host.pump(now);
    client.pump(now);
    observer.pump(now);
  };
  ASSERT_TRUE(pump_until(pump, [&] {
    return host.snapshot().players.size() == 3 && client.snapshot().players.size() == 3 &&
           observer.snapshot().players.size() == 3;
  }));
  host.enter_lobby();
  ASSERT_TRUE(client.set_ready(true));
  ASSERT_TRUE(pump_until(pump, [&] {
    return std::ranges::any_of(host.snapshot().players, [&](const auto& profile) {
      return profile.player_id == client.local_profile().player_id && profile.ready;
    });
  }));
  auto updated = client.local_profile();
  updated.character = PlayerCharacter::DAXTER;
  EXPECT_FALSE(client.set_local_profile(updated));
  ASSERT_TRUE(host.start_game());
  ASSERT_TRUE(pump_until(pump, [&] {
    return client.snapshot().state.status == SessionStatus::GAME_STARTING &&
           observer.snapshot().state.status == SessionStatus::GAME_STARTING;
  }));
  host.enter_game();
  client.enter_game();
  observer.enter_game();
  EXPECT_FALSE(client.set_local_profile(updated));
  host_adapter.descriptor_.allow_in_game_character_changes = true;
  client_adapter.descriptor_.allow_in_game_character_changes = true;
  observer_adapter.descriptor_.allow_in_game_character_changes = true;
  updated.display_name = "Updated";
  updated.game_extension = {1, 2, 3};
  ASSERT_TRUE(client.set_local_profile(updated));
  const auto matches = [&](const SessionController& controller) {
    return std::ranges::any_of(controller.snapshot().players, [&](const auto& value) {
      return value.player_id == updated.player_id && value.display_name == updated.display_name &&
             value.character == updated.character && value.game_extension == updated.game_extension;
    });
  };
  ASSERT_TRUE(
      pump_until(pump, [&] { return matches(host) && matches(client) && matches(observer); }));
  auto host_profile = host.local_profile();
  host_profile.character = PlayerCharacter::DAXTER;
  host_profile.display_name = "NewHost";
  ASSERT_TRUE(host.set_local_profile(host_profile));
  ASSERT_TRUE(pump_until(pump, [&] {
    return std::ranges::any_of(observer.snapshot().players, [](const auto& value) {
      return value.player_id == 0 && value.display_name == "NewHost" &&
             value.character == PlayerCharacter::DAXTER;
    });
  }));
}
