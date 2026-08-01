#ifdef _WIN32

#include "tools/multiplayer_netem/netem_core.h"
#include "tools/multiplayer_netem/netem_relay.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include "gtest/gtest.h"

namespace {

using multiplayer_netem::Direction;
using multiplayer_netem::DirectionSettings;
using multiplayer_netem::EndpointRouter;
using multiplayer_netem::ImpairmentModel;
using multiplayer_netem::NetemClock;
using multiplayer_netem::PacketQueue;
using multiplayer_netem::QueuedDatagram;
using multiplayer_netem::RelayConfig;
using multiplayer_netem::UdpRelay;

sockaddr_in loopback_endpoint(uint16_t port) {
  sockaddr_in endpoint = {};
  endpoint.sin_family = AF_INET;
  endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  endpoint.sin_port = htons(port);
  return endpoint;
}

SOCKET bind_ephemeral(sockaddr_in& endpoint) {
  const SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_handle == INVALID_SOCKET) {
    return INVALID_SOCKET;
  }
  const sockaddr_in requested = loopback_endpoint(0);
  if (bind(socket_handle, reinterpret_cast<const sockaddr*>(&requested), sizeof(requested)) != 0) {
    closesocket(socket_handle);
    return INVALID_SOCKET;
  }
  int endpoint_size = sizeof(endpoint);
  if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&endpoint), &endpoint_size) != 0) {
    closesocket(socket_handle);
    return INVALID_SOCKET;
  }
  return socket_handle;
}

bool send_text(SOCKET socket_handle, const sockaddr_in& destination, std::string_view text) {
  const int sent = sendto(socket_handle, text.data(), static_cast<int>(text.size()), 0,
                          reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
  return sent == static_cast<int>(text.size());
}

std::string receive_text(SOCKET socket_handle, sockaddr_in& source, int timeout_ms) {
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(socket_handle, &read_set);
  timeval timeout = {};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  if (select(0, &read_set, nullptr, nullptr, &timeout) <= 0) {
    return {};
  }

  std::array<char, 256> buffer = {};
  int source_size = sizeof(source);
  const int received = recvfrom(socket_handle, buffer.data(), static_cast<int>(buffer.size()), 0,
                                reinterpret_cast<sockaddr*>(&source), &source_size);
  if (received <= 0) {
    return {};
  }
  return std::string(buffer.data(), static_cast<size_t>(received));
}

class WinsockScope {
 public:
  WinsockScope() : ready(WSAStartup(MAKEWORD(2, 2), &data) == 0) {}
  ~WinsockScope() {
    if (ready) {
      WSACleanup();
    }
  }

  bool ready = false;
  WSADATA data = {};
};

DirectionSettings no_impairment() {
  return DirectionSettings{};
}

}  // namespace

TEST(MultiplayerNetemProfile, IncludesRealisticProfilesAndRejectsUnknownNames) {
  const auto wifi = multiplayer_netem::find_profile("wifi");
  ASSERT_TRUE(wifi.has_value());
  EXPECT_EQ(wifi->name, "wifi");
  EXPECT_EQ(wifi->client_to_host.latency_ms, 25);
  EXPECT_EQ(wifi->host_to_client.latency_ms, 25);
  EXPECT_GT(wifi->client_to_host.jitter_ms, 0);
  EXPECT_GT(wifi->client_to_host.loss_percent, 0.0);
  EXPECT_TRUE(multiplayer_netem::find_profile("lan").has_value());
  EXPECT_TRUE(multiplayer_netem::find_profile("4g").has_value());
  EXPECT_TRUE(multiplayer_netem::find_profile("poor-4g").has_value());
  EXPECT_TRUE(multiplayer_netem::find_profile("stress").has_value());
  EXPECT_FALSE(multiplayer_netem::find_profile("unknown").has_value());
}

TEST(MultiplayerNetemEndpoint, ParsesIpv4EndpointAndRejectsInvalidPorts) {
  sockaddr_in endpoint = {};
  std::string error;
  ASSERT_TRUE(multiplayer_netem::parse_endpoint("127.0.0.1:26212", endpoint, error));
  EXPECT_EQ(ntohs(endpoint.sin_port), 26212);
  EXPECT_EQ(multiplayer_netem::endpoint_to_string(endpoint), "127.0.0.1:26212");
  EXPECT_FALSE(multiplayer_netem::parse_endpoint("127.0.0.1:0", endpoint, error));
  EXPECT_FALSE(multiplayer_netem::parse_endpoint("localhost:26212", endpoint, error));
  EXPECT_FALSE(multiplayer_netem::parse_endpoint("127.0.0.1", endpoint, error));
}

TEST(MultiplayerNetemImpairment, SameSeedProducesSameLossAndDelayDecisions) {
  DirectionSettings settings;
  settings.latency_ms = 40;
  settings.jitter_ms = 12;
  settings.loss_percent = 17.0;
  settings.burst_length = 2;
  settings.reorder_percent = 9.0;
  settings.reorder_delay_ms = 30;
  settings.duplicate_percent = 4.0;
  ImpairmentModel first(settings, 1234);
  ImpairmentModel second(settings, 1234);

  for (int index = 0; index < 128; ++index) {
    const auto first_result = first.apply();
    const auto second_result = second.apply();
    EXPECT_EQ(first_result.dropped, second_result.dropped);
    EXPECT_EQ(first_result.duplicated, second_result.duplicated);
    EXPECT_EQ(first_result.delay_ms, second_result.delay_ms);
  }
}

TEST(MultiplayerNetemImpairment, JitterStaysWithinConfiguredBounds) {
  DirectionSettings settings;
  settings.latency_ms = 50;
  settings.jitter_ms = 20;
  ImpairmentModel model(settings, 42);
  for (int index = 0; index < 512; ++index) {
    const auto result = model.apply();
    ASSERT_FALSE(result.dropped);
    EXPECT_GE(result.delay_ms, 30u);
    EXPECT_LE(result.delay_ms, 70u);
  }
}

TEST(MultiplayerNetemImpairment, BurstLossKeepsDroppingDuringConfiguredBurst) {
  DirectionSettings settings;
  settings.loss_percent = 100.0;
  settings.burst_length = 3;
  ImpairmentModel model(settings, 7);
  for (int index = 0; index < 8; ++index) {
    EXPECT_TRUE(model.apply().dropped);
  }
}

TEST(MultiplayerNetemImpairment, ReorderingAndDuplicationAddConfiguredEffects) {
  DirectionSettings settings;
  settings.latency_ms = 10;
  settings.reorder_percent = 100.0;
  settings.reorder_delay_ms = 25;
  settings.duplicate_percent = 100.0;
  ImpairmentModel model(settings, 9);
  const auto result = model.apply();
  EXPECT_FALSE(result.dropped);
  EXPECT_TRUE(result.duplicated);
  EXPECT_EQ(result.delay_ms, 35u);
}

TEST(MultiplayerNetemQueue, EnforcesPacketAndByteLimits) {
  PacketQueue queue(2, 5);
  const auto now = NetemClock::now();
  QueuedDatagram first;
  first.payload = {1, 2, 3};
  first.release_at = now;
  first.direction = Direction::ClientToHost;
  EXPECT_TRUE(queue.push(std::move(first)));

  QueuedDatagram too_many_bytes;
  too_many_bytes.payload = {4, 5, 6};
  too_many_bytes.release_at = now;
  EXPECT_FALSE(queue.push(std::move(too_many_bytes)));
  EXPECT_EQ(queue.packet_count(), 1u);
  EXPECT_EQ(queue.byte_count(), 3u);

  QueuedDatagram second;
  second.payload = {7, 8};
  second.release_at = now;
  EXPECT_TRUE(queue.push(std::move(second)));
  EXPECT_EQ(queue.packet_count(), 2u);
  EXPECT_EQ(queue.byte_count(), 5u);

  QueuedDatagram too_many_packets;
  too_many_packets.payload = {9};
  too_many_packets.release_at = now;
  EXPECT_FALSE(queue.push(std::move(too_many_packets)));
  EXPECT_EQ(queue.take_due(now).size(), 2u);
  EXPECT_EQ(queue.packet_count(), 0u);
  EXPECT_EQ(queue.byte_count(), 0u);
}

TEST(MultiplayerNetemRouting, LearnsOneClientAndRejectsUnexpectedEndpoints) {
  const sockaddr_in host = loopback_endpoint(26210);
  const sockaddr_in client = loopback_endpoint(31001);
  const sockaddr_in other = loopback_endpoint(31002);
  EndpointRouter router(host);

  EXPECT_EQ(router.route_for(client), EndpointRouter::Route::ClientToHost);
  EXPECT_EQ(router.route_for(client), EndpointRouter::Route::ClientToHost);
  EXPECT_EQ(router.route_for(host), EndpointRouter::Route::HostToClient);
  EXPECT_EQ(router.route_for(other), EndpointRouter::Route::Ignored);
  ASSERT_TRUE(router.client_endpoint().has_value());
  EXPECT_TRUE(multiplayer_netem::same_endpoint(*router.client_endpoint(), client));
}

TEST(MultiplayerNetemRelay, PassesUdpInBothDirectionsWithoutImpairment) {
  WinsockScope winsock;
  ASSERT_TRUE(winsock.ready);

  sockaddr_in host_endpoint = {};
  sockaddr_in client_endpoint = {};
  const SOCKET host_socket = bind_ephemeral(host_endpoint);
  const SOCKET client_socket = bind_ephemeral(client_endpoint);
  ASSERT_NE(host_socket, INVALID_SOCKET);
  ASSERT_NE(client_socket, INVALID_SOCKET);

  RelayConfig config;
  config.listen_port = 0;
  config.target = host_endpoint;
  config.client_to_host = no_impairment();
  config.host_to_client = no_impairment();
  config.max_queue_packets = 32;
  config.max_queue_bytes = 64 * 1024;
  UdpRelay relay(config);
  std::atomic_bool stop_requested = false;
  std::atomic_int run_result = -1;
  std::ostringstream log;
  std::thread relay_thread([&] { run_result = relay.run(stop_requested, log); });

  uint16_t relay_port = 0;
  for (int attempt = 0; attempt < 200 && relay_port == 0; ++attempt) {
    relay_port = relay.bound_port();
    if (relay_port == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  bool forward_ok = false;
  bool reverse_ok = false;
  if (relay_port != 0) {
    const sockaddr_in relay_endpoint = loopback_endpoint(relay_port);
    forward_ok = send_text(client_socket, relay_endpoint, "ping");
    sockaddr_in relay_source = {};
    forward_ok = forward_ok && receive_text(host_socket, relay_source, 2000) == "ping";
    if (forward_ok) {
      reverse_ok = send_text(host_socket, relay_source, "pong");
      sockaddr_in client_source = {};
      reverse_ok = reverse_ok && receive_text(client_socket, client_source, 2000) == "pong";
    }
  }

  stop_requested = true;
  relay_thread.join();
  closesocket(host_socket);
  closesocket(client_socket);

  EXPECT_NE(relay_port, 0);
  EXPECT_TRUE(forward_ok);
  EXPECT_TRUE(reverse_ok);
  EXPECT_EQ(run_result.load(), 0);
  EXPECT_EQ(relay.stats().client_to_host.forwarded_packets, 1u);
  EXPECT_EQ(relay.stats().host_to_client.forwarded_packets, 1u);
  EXPECT_EQ(relay.stats().client_to_host.received_bytes, 4u);
  EXPECT_EQ(relay.stats().host_to_client.forwarded_bytes, 4u);
}

#endif
