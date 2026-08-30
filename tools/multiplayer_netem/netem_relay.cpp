#ifdef _WIN32

#include "tools/multiplayer_netem/netem_relay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <ws2tcpip.h>

namespace multiplayer_netem {
namespace {

constexpr auto kSelectInterval = std::chrono::milliseconds(10);
constexpr auto kStatsInterval = std::chrono::seconds(1);
constexpr auto kClientEndpointRebindIdle = std::chrono::milliseconds(500);
constexpr auto kClientEndpointStateIdle = std::chrono::seconds(30);
constexpr size_t kMaximumRetiredClients = 16;

sockaddr_in make_loopback_endpoint(uint16_t port) {
  sockaddr_in endpoint = {};
  endpoint.sin_family = AF_INET;
  endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  endpoint.sin_port = htons(port);
  return endpoint;
}

bool set_nonblocking(SOCKET socket, std::ostream& log) {
  u_long nonblocking = 1;
  if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
    log << "[Netem] Failed to make relay socket non-blocking: " << WSAGetLastError() << '\n';
    return false;
  }
  return true;
}

}  // namespace

bool parse_endpoint(std::string_view text, sockaddr_in& endpoint, std::string& error) {
  const size_t separator = text.rfind(':');
  if (separator == std::string_view::npos || separator == 0 || separator + 1 >= text.size()) {
    error = "endpoint must use IPv4:PORT syntax";
    return false;
  }

  const std::string address(text.substr(0, separator));
  const std::string port_text(text.substr(separator + 1));
  char* end = nullptr;
  const unsigned long port = std::strtoul(port_text.c_str(), &end, 10);
  if (!end || *end != '\0' || port == 0 || port > UINT16_MAX) {
    error = "endpoint port is invalid";
    return false;
  }

  endpoint = {};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(static_cast<uint16_t>(port));
  if (InetPtonA(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
    error = "endpoint address must be a valid IPv4 address";
    return false;
  }
  return true;
}

std::string endpoint_to_string(const sockaddr_in& endpoint) {
  char address[INET_ADDRSTRLEN] = {};
  if (!InetNtopA(AF_INET, const_cast<IN_ADDR*>(&endpoint.sin_addr), address,
                 static_cast<DWORD>(sizeof(address)))) {
    return "<invalid>";
  }
  return std::string(address) + ":" + std::to_string(ntohs(endpoint.sin_port));
}

bool same_endpoint(const sockaddr_in& left, const sockaddr_in& right) {
  return left.sin_family == right.sin_family && left.sin_addr.s_addr == right.sin_addr.s_addr &&
         left.sin_port == right.sin_port;
}

bool is_transient_receive_error(int error) {
  return error == WSAEWOULDBLOCK || error == WSAEINTR || error == WSAECONNRESET;
}

EndpointRouter::EndpointRouter(sockaddr_in target) : m_target(target) {}

EndpointRouter::Route EndpointRouter::route_for(const sockaddr_in& source) {
  return route_for(source, NetemClock::now());
}

EndpointRouter::Route EndpointRouter::route_for(const sockaddr_in& source, NetemTimePoint now) {
  if (same_endpoint(source, m_target)) {
    return m_client ? Route::HostToClient : Route::Ignored;
  }

  if (!m_client) {
    m_client = ClientEndpoint{source, now};
    return Route::ClientToHost;
  }

  if (same_endpoint(source, m_client->endpoint)) {
    m_client->last_seen = now;
    return Route::ClientToHost;
  }

  for (auto retired = m_retired_clients.begin(); retired != m_retired_clients.end(); ++retired) {
    if (!same_endpoint(source, retired->endpoint)) {
      continue;
    }

    if (now - m_client->last_seen < kClientEndpointRebindIdle) {
      return Route::Ignored;
    }

    m_retired_clients.erase(retired);
    break;
  }

  if (m_retired_clients.size() >= kMaximumRetiredClients) {
    m_retired_clients.erase(m_retired_clients.begin());
  }
  m_retired_clients.push_back(RetiredClient{m_client->endpoint});
  m_client = ClientEndpoint{source, now};
  return Route::ClientToHost;
}

bool EndpointRouter::expire_idle(NetemTimePoint now) {
  if (!m_client || now - m_client->last_seen < kClientEndpointStateIdle) {
    return false;
  }
  m_client.reset();
  m_retired_clients.clear();
  return true;
}

std::optional<sockaddr_in> EndpointRouter::client_endpoint() const {
  if (!m_client) {
    return std::nullopt;
  }
  return m_client->endpoint;
}

bool EndpointRouter::is_retired_endpoint(const sockaddr_in& endpoint) const {
  for (const auto& retired : m_retired_clients) {
    if (same_endpoint(endpoint, retired.endpoint)) {
      return true;
    }
  }
  return false;
}

size_t EndpointRouter::retired_endpoint_count() const {
  return m_retired_clients.size();
}

UdpRelay::UdpRelay(RelayConfig config)
    : m_config(std::move(config)),
      m_router(m_config.target),
      m_client_to_host_model(m_config.client_to_host, m_config.seed),
      m_host_to_client_model(m_config.host_to_client, m_config.seed ^ UINT64_C(0x9e3779b97f4a7c15)),
      m_queue(m_config.max_queue_packets, m_config.max_queue_bytes) {}

bool UdpRelay::open_socket(std::ostream& log) {
  m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (m_socket == INVALID_SOCKET) {
    log << "[Netem] Failed to create relay socket: " << WSAGetLastError() << '\n';
    return false;
  }
  if (!set_nonblocking(m_socket, log)) {
    closesocket(m_socket);
    m_socket = INVALID_SOCKET;
    return false;
  }

  const sockaddr_in listen_endpoint = make_loopback_endpoint(m_config.listen_port);
  if (bind(m_socket, reinterpret_cast<const sockaddr*>(&listen_endpoint), sizeof(listen_endpoint)) !=
      0) {
    log << "[Netem] Failed to bind relay on 127.0.0.1:" << m_config.listen_port << ": "
        << WSAGetLastError() << '\n';
    closesocket(m_socket);
    m_socket = INVALID_SOCKET;
    return false;
  }

  sockaddr_in bound_endpoint = {};
  int endpoint_size = sizeof(bound_endpoint);
  if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&bound_endpoint), &endpoint_size) != 0) {
    log << "[Netem] Failed to query relay port: " << WSAGetLastError() << '\n';
    closesocket(m_socket);
    m_socket = INVALID_SOCKET;
    return false;
  }
  m_bound_port = ntohs(bound_endpoint.sin_port);
  return true;
}

DirectionStats& UdpRelay::direction_stats(Direction direction) {
  return direction == Direction::ClientToHost ? m_stats.client_to_host : m_stats.host_to_client;
}

ImpairmentModel& UdpRelay::direction_model(Direction direction) {
  return direction == Direction::ClientToHost ? m_client_to_host_model : m_host_to_client_model;
}

bool UdpRelay::schedule_packet(Direction direction,
                               const uint8_t* payload,
                               size_t payload_size,
                               const ImpairmentResult& impairment,
                               NetemTimePoint now) {
  auto& stats = direction_stats(direction);
  QueuedDatagram datagram;
  datagram.payload.assign(payload, payload + payload_size);
  datagram.release_at = now + std::chrono::milliseconds(impairment.delay_ms);
  datagram.direction = direction;
  if (!m_queue.push(std::move(datagram))) {
    ++stats.queue_dropped_packets;
    return false;
  }
  return true;
}

bool UdpRelay::receive_available(std::ostream& log) {
  std::array<uint8_t, kMaximumDatagramSize> receive_buffer = {};
  while (true) {
    sockaddr_in source = {};
    int source_size = sizeof(source);
    const int received = recvfrom(m_socket, reinterpret_cast<char*>(receive_buffer.data()),
                                  static_cast<int>(receive_buffer.size()), 0,
                                  reinterpret_cast<sockaddr*>(&source), &source_size);
    if (received == SOCKET_ERROR) {
      const int error = WSAGetLastError();
      if (is_transient_receive_error(error)) {
        const auto now = NetemClock::now();
        if (error == WSAECONNRESET &&
            (m_last_receive_reset_log_time == NetemTimePoint{} ||
             now - m_last_receive_reset_log_time >= kStatsInterval)) {
          log << "[Netem] recvfrom transient reset: " << error << "; continuing relay.\n";
          log.flush();
          m_last_receive_reset_log_time = now;
        }
        return true;
      }
      log << "[Netem] recvfrom failed: " << error << '\n';
      return false;
    }
    if (received <= 0) {
      continue;
    }

    const auto now = NetemClock::now();
    const auto previous_client = m_router.client_endpoint();
    const bool source_was_retired = m_router.is_retired_endpoint(source);
    const auto route = m_router.route_for(source, now);
    const auto current_client = m_router.client_endpoint();
    if (route == EndpointRouter::Route::ClientToHost && current_client &&
        (!previous_client || !same_endpoint(*previous_client, *current_client))) {
      if (!previous_client) {
        log << "[Netem] client endpoint learned: " << endpoint_to_string(*current_client)
            << '\n';
      } else {
        log << "[Netem] client endpoint " << (source_was_retired ? "reused" : "migrated")
            << ": " << endpoint_to_string(*previous_client) << " -> "
            << endpoint_to_string(*current_client) << '\n';
      }
      log.flush();
    }

    Direction direction;
    switch (route) {
      case EndpointRouter::Route::ClientToHost:
        direction = Direction::ClientToHost;
        break;
      case EndpointRouter::Route::HostToClient: {
        const auto client = m_router.client_endpoint();
        if (!client) {
          ++m_stats.ignored_packets;
          continue;
        }
        direction = Direction::HostToClient;
        break;
      }
      case EndpointRouter::Route::Ignored:
        ++m_stats.ignored_packets;
        if (m_last_ignored_log_time == NetemTimePoint{} ||
            now - m_last_ignored_log_time >= kStatsInterval) {
          log << "[Netem] ignored endpoint " << endpoint_to_string(source) << " (current="
              << (current_client ? endpoint_to_string(*current_client) : "<none>")
              << ", ignored=" << m_stats.ignored_packets << ")\n";
          log.flush();
          m_last_ignored_log_time = now;
        }
        continue;
    }

    auto& stats = direction_stats(direction);
    stats.received_packets++;
    stats.received_bytes += static_cast<uint64_t>(received);
    const auto impairment = direction_model(direction).apply();
    if (impairment.dropped) {
      ++stats.dropped_packets;
      continue;
    }

    schedule_packet(direction, receive_buffer.data(), static_cast<size_t>(received), impairment,
                    now);
    if (impairment.duplicated) {
      ++stats.duplicated_packets;
      schedule_packet(direction, receive_buffer.data(), static_cast<size_t>(received), impairment,
                      now);
    }
  }
}

void UdpRelay::send_due_packets(std::ostream& log, NetemTimePoint now) {
  for (auto& datagram : m_queue.take_due(now)) {
    std::optional<sockaddr_in> destination;
    if (datagram.direction == Direction::ClientToHost) {
      destination = m_config.target;
    } else {
      destination = m_router.client_endpoint();
    }
    if (!destination) {
      ++m_stats.ignored_packets;
      continue;
    }
    const int sent = sendto(m_socket, reinterpret_cast<const char*>(datagram.payload.data()),
                            static_cast<int>(datagram.payload.size()), 0,
                            reinterpret_cast<const sockaddr*>(&*destination), sizeof(*destination));
    if (sent == SOCKET_ERROR) {
      ++m_stats.send_errors;
      log << "[Netem] sendto failed: " << WSAGetLastError() << '\n';
      continue;
    }
    auto& stats = direction_stats(datagram.direction);
    ++stats.forwarded_packets;
    stats.forwarded_bytes += static_cast<uint64_t>(sent);
  }
}

void UdpRelay::log_stats(std::ostream& log) const {
  const auto& upload = m_stats.client_to_host;
  const auto& download = m_stats.host_to_client;
  const auto client = m_router.client_endpoint();
  log << "[Netem] stats upload rx=" << upload.received_packets << " tx="
      << upload.forwarded_packets << " loss=" << upload.dropped_packets << " dup="
      << upload.duplicated_packets << " queue-drop=" << upload.queue_dropped_packets
      << "; download rx=" << download.received_packets << " tx=" << download.forwarded_packets
      << " loss=" << download.dropped_packets << " dup=" << download.duplicated_packets
      << " queue-drop=" << download.queue_dropped_packets << "; ignored="
      << m_stats.ignored_packets << " send-errors=" << m_stats.send_errors
      << " queued=" << m_queue.packet_count() << "/" << m_config.max_queue_packets
      << " client=" << (client ? endpoint_to_string(*client) : "<none>") << '\n';
  log.flush();
}

int UdpRelay::run(std::atomic_bool& stop_requested, std::ostream& log) {
  WSADATA winsock = {};
  if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
    log << "[Netem] WSAStartup failed.\n";
    return 1;
  }

  const bool opened = open_socket(log);
  if (!opened) {
    WSACleanup();
    return 1;
  }
  log << "[Netem] listening on 127.0.0.1:" << bound_port() << " -> "
      << endpoint_to_string(m_config.target) << '\n';
  log.flush();

  auto next_stats = NetemClock::now() + kStatsInterval;
  while (!stop_requested.load()) {
    const auto before_select = NetemClock::now();
    if (m_router.expire_idle(before_select)) {
      log << "[Netem] client endpoint state expired after idle timeout; accepting a fresh client endpoint.\n";
      log.flush();
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(m_socket, &read_set);
    timeval timeout = {};
    timeout.tv_sec = 0;
    timeout.tv_usec = static_cast<long>(kSelectInterval.count()) * 1000;
    const int selected = select(0, &read_set, nullptr, nullptr, &timeout);
    if (selected == SOCKET_ERROR) {
      log << "[Netem] select failed: " << WSAGetLastError() << '\n';
      break;
    }
    if (selected > 0 && FD_ISSET(m_socket, &read_set) && !receive_available(log)) {
      break;
    }
    const auto now = NetemClock::now();
    send_due_packets(log, now);
    if (now >= next_stats) {
      log_stats(log);
      next_stats = now + kStatsInterval;
    }
  }

  send_due_packets(log, NetemClock::now());
  log_stats(log);
  closesocket(m_socket);
  m_socket = INVALID_SOCKET;
  m_bound_port = 0;
  WSACleanup();
  return 0;
}

uint16_t UdpRelay::bound_port() const {
  return m_bound_port.load();
}

const RelayStats& UdpRelay::stats() const {
  return m_stats;
}

}  // namespace multiplayer_netem

#endif
