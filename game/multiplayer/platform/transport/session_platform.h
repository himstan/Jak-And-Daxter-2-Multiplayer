#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "game/multiplayer/platform/core/network_statistics.h"
#include "game/multiplayer/platform/core/types.h"

namespace multiplayer::platform {

inline constexpr size_t kMaximumTransportMessagesPerPump = 256;

struct TransportHostConfig {
  uint16_t port = 0;
  size_t maximum_pending_connections = 64;
};

struct TransportClientConfig {
  std::string endpoint;
  uint16_t port = 0;
};

enum class TransportEventKind : uint8_t {
  CONNECTED,
  MESSAGE,
  CLOSED,
};

struct TransportEvent {
  TransportEventKind kind = TransportEventKind::CLOSED;
  ConnectionId connection_id = 0;
  Delivery delivery = Delivery::UNRELIABLE_REALTIME;
  TransportLane lane = TransportLane::REALTIME_NORMAL;
  int close_reason = 0;
  std::string detail;
  std::string remote_address;
  std::vector<uint8_t> payload;
};

class SessionPlatform {
 public:
  struct Impl;

  SessionPlatform();
  ~SessionPlatform();
  SessionPlatform(const SessionPlatform&) = delete;
  SessionPlatform& operator=(const SessionPlatform&) = delete;

  bool host(const TransportHostConfig& config) const;
  bool connect(const TransportClientConfig& config) const;
  void shutdown(int reason = 0, bool graceful = false) const;
  void pump() const;
  bool poll_event(TransportEvent& event) const;
  bool send(ConnectionId connection, std::span<const uint8_t> payload, Delivery delivery) const;
  bool send(ConnectionId connection, std::span<const uint8_t> payload, TransportLane lane) const;
  void close_connection(ConnectionId connection, int reason, std::string_view detail = {}) const;

  bool active() const;
  bool is_host() const;
  uint16_t local_port() const;
  ConnectionId host_connection_id() const;
  std::vector<ConnectionSnapshot> connection_snapshots() const;
  AggregateSnapshot aggregate_snapshot() const;

 private:
  std::unique_ptr<Impl> impl_;
};

uint64_t transport_error_count();

}  // namespace multiplayer::platform
