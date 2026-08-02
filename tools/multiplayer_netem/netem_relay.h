#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "tools/multiplayer_netem/netem_core.h"

namespace multiplayer_netem {

bool parse_endpoint(std::string_view text, sockaddr_in& endpoint, std::string& error);
std::string endpoint_to_string(const sockaddr_in& endpoint);
bool same_endpoint(const sockaddr_in& left, const sockaddr_in& right);
bool is_transient_receive_error(int error);

class EndpointRouter {
 public:
  enum class Route {
    ClientToHost,
    HostToClient,
    Ignored,
  };

  explicit EndpointRouter(sockaddr_in target);

  Route route_for(const sockaddr_in& source);
  Route route_for(const sockaddr_in& source, NetemTimePoint now);
  std::optional<sockaddr_in> client_endpoint() const;
  bool is_retired_endpoint(const sockaddr_in& endpoint) const;

 private:
  struct ClientEndpoint {
    sockaddr_in endpoint = {};
    NetemTimePoint last_seen;
  };

  struct RetiredClient {
    sockaddr_in endpoint = {};
  };

  sockaddr_in m_target;
  std::optional<ClientEndpoint> m_client;
  std::vector<RetiredClient> m_retired_clients;
};

struct RelayConfig {
  uint16_t listen_port = 26212;
  sockaddr_in target = {};
  DirectionSettings client_to_host;
  DirectionSettings host_to_client;
  uint64_t seed = 1;
  size_t max_queue_packets = 4096;
  size_t max_queue_bytes = 4 * 1024 * 1024;
};

class UdpRelay {
 public:
  explicit UdpRelay(RelayConfig config);

  int run(std::atomic_bool& stop_requested, std::ostream& log);
  uint16_t bound_port() const;
  const RelayStats& stats() const;

 private:
  static constexpr size_t kMaximumDatagramSize = 65507;

  bool open_socket(std::ostream& log);
  bool receive_available(std::ostream& log);
  bool schedule_packet(Direction direction,
                       const uint8_t* payload,
                       size_t payload_size,
                       const ImpairmentResult& impairment,
                       NetemTimePoint now);
  void send_due_packets(std::ostream& log, NetemTimePoint now);
  void log_stats(std::ostream& log) const;
  DirectionStats& direction_stats(Direction direction);
  ImpairmentModel& direction_model(Direction direction);

  RelayConfig m_config;
  SOCKET m_socket = INVALID_SOCKET;
  std::atomic<uint16_t> m_bound_port = 0;
  EndpointRouter m_router;
  ImpairmentModel m_client_to_host_model;
  ImpairmentModel m_host_to_client_model;
  PacketQueue m_queue;
  RelayStats m_stats;
  NetemTimePoint m_last_ignored_log_time;
  NetemTimePoint m_last_receive_reset_log_time;
};

}  // namespace multiplayer_netem

#endif
