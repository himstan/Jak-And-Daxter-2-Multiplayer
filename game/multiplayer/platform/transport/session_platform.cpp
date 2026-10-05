#include "game/multiplayer/platform/transport/session_platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "common/log/log.h"

#include "game/multiplayer/platform/core/limits.h"
#include "steam/isteamnetworkingutils.h"
#include "steam/steamnetworkingsockets.h"

namespace multiplayer::platform {
namespace {
constexpr int kCloseTransportSetup = 4000;
constexpr int kCloseReliableBacklog = 4001;
constexpr auto kGracefulShutdownTimeout = std::chrono::milliseconds(500);
constexpr int kLaneCount = 5;
constexpr size_t kReceiveBatchSize = 32;

Delivery lane_delivery(const TransportLane lane) {
  return lane == TransportLane::CONTROL_RELIABLE || lane == TransportLane::GAMEPLAY_RELIABLE
             ? Delivery::RELIABLE_ORDERED
             : Delivery::UNRELIABLE_REALTIME;
}

Delivery message_delivery(const int flags) {
  return (flags & k_nSteamNetworkingSend_Reliable) != 0 ? Delivery::RELIABLE_ORDERED
                                                        : Delivery::UNRELIABLE_REALTIME;
}

bool valid_lane(const int lane) {
  return lane >= 0 && lane < kLaneCount;
}

std::mutex g_init_mutex;
size_t g_init_references = 0;
bool g_init_ok = false;
std::vector<SessionPlatform::Impl*> g_instances;
std::atomic_uint64_t g_transport_error_count = 0;

void gns_debug_output(const ESteamNetworkingSocketsDebugOutputType type, const char* message) {
  if (type <= k_ESteamNetworkingSocketsDebugOutputType_Error) {
    ++g_transport_error_count;
    lg::error("[GNS] {}", message);
  } else if (type == k_ESteamNetworkingSocketsDebugOutputType_Warning)
    lg::warn("[GNS] {}", message);
  else
    lg::info("[GNS] {}", message);
}

bool acquire_gns() {
  std::lock_guard lock(g_init_mutex);
  if (g_init_references++ == 0) {
    SteamDatagramErrMsg error = {};
    g_init_ok = GameNetworkingSockets_Init(nullptr, error);
    if (g_init_ok) {
      SteamNetworkingUtils()->SetDebugOutputFunction(
          k_ESteamNetworkingSocketsDebugOutputType_Warning, gns_debug_output);
    } else {
      lg::error("[SessionPlatform] GameNetworkingSockets_Init failed: {}", error);
    }
  }
  return g_init_ok;
}

void release_gns() {
  std::lock_guard lock(g_init_mutex);
  if (g_init_references != 0 && --g_init_references == 0) {
    if (g_init_ok) {
      SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_None,
                                                     nullptr);
      GameNetworkingSockets_Kill();
    }
    g_init_ok = false;
  }
}
}  // namespace

struct SessionPlatform::Impl {
  struct Connection {
    HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
    ConnectionId id = 0;
    bool connected = false;
    bool lanes_configured = false;
    std::string remote_address;
  };

  ISteamNetworkingSockets* sockets = nullptr;
  HSteamListenSocket listen_socket = k_HSteamListenSocket_Invalid;
  HSteamNetPollGroup poll_group = k_HSteamNetPollGroup_Invalid;
  HSteamNetConnection host_connection = k_HSteamNetConnection_Invalid;
  bool hosting = false;
  bool running = false;
  size_t maximum_pending_connections = 16;
  ConnectionId next_connection_id = 1;
  std::unordered_map<HSteamNetConnection, Connection> gns_connections;
  std::unordered_map<ConnectionId, HSteamNetConnection> connection_handles;
  std::deque<TransportEvent> events;

  Impl() {
    if (acquire_gns())
      sockets = SteamNetworkingSockets();
    std::lock_guard lock(g_init_mutex);
    g_instances.push_back(this);
  }

  ~Impl() {
    shutdown(0);
    {
      std::lock_guard lock(g_init_mutex);
      std::erase(g_instances, this);
    }
    release_gns();
  }

  bool owns(const SteamNetConnectionStatusChangedCallback_t& info) const {
    return gns_connections.contains(info.m_hConn) ||
           (listen_socket != k_HSteamListenSocket_Invalid &&
            info.m_info.m_hListenSocket == listen_socket);
  }

  static void connection_callback(const SteamNetConnectionStatusChangedCallback_t* info) {
    std::lock_guard lock(g_init_mutex);
    for (auto* instance : g_instances) {
      if (instance->owns(*info)) {
        instance->on_connection_changed(*info);
        return;
      }
    }
  }

  static std::string address_string(const SteamNetworkingIPAddr& address) {
    char text[SteamNetworkingIPAddr::k_cchMaxString] = {};
    address.ToString(text, sizeof(text), false);
    return text;
  }

  bool configure_connection(const HSteamNetConnection connection) const {
    const int priorities[kLaneCount] = {0, 0, 1, 1, 2};
    const uint16 weights[kLaneCount] = {1, 4, 1, 1, 1};
    return sockets->ConfigureConnectionLanes(connection, kLaneCount, priorities, weights) ==
           k_EResultOK;
  }

  Connection& add_connection(const HSteamNetConnection connection,
                             const SteamNetworkingIPAddr* remote = nullptr) {
    auto [found, inserted] = gns_connections.try_emplace(connection);
    if (inserted) {
      found->second.connection = connection;
      found->second.id = next_connection_id++;
      connection_handles[found->second.id] = connection;
    }
    if (remote)
      found->second.remote_address = address_string(*remote);
    return found->second;
  }

  void remove_connection(const HSteamNetConnection connection) {
    const auto found = gns_connections.find(connection);
    if (found == gns_connections.end())
      return;
    connection_handles.erase(found->second.id);
    gns_connections.erase(found);
  }

  void on_connection_changed(const SteamNetConnectionStatusChangedCallback_t& callback) {
    const auto state = callback.m_info.m_eState;
    if (state == k_ESteamNetworkingConnectionState_Connecting && hosting) {
      if (!gns_connections.contains(callback.m_hConn) &&
          gns_connections.size() >= maximum_pending_connections) {
        sockets->CloseConnection(callback.m_hConn, kCloseTransportSetup,
                                 "transport capacity exceeded", false);
        return;
      }
      auto& connection = add_connection(callback.m_hConn, &callback.m_info.m_addrRemote);
      connection.lanes_configured = configure_connection(callback.m_hConn);
      if (!connection.lanes_configured ||
          sockets->AcceptConnection(callback.m_hConn) != k_EResultOK ||
          !sockets->SetConnectionPollGroup(callback.m_hConn, poll_group)) {
        sockets->CloseConnection(callback.m_hConn, kCloseTransportSetup, "transport setup failed",
                                 false);
        remove_connection(callback.m_hConn);
      }
      return;
    }
    if (state == k_ESteamNetworkingConnectionState_Connected) {
      auto& connection = add_connection(callback.m_hConn, &callback.m_info.m_addrRemote);
      if (!((connection.lanes_configured = configure_connection(callback.m_hConn)))) {
        sockets->CloseConnection(callback.m_hConn, kCloseTransportSetup,
                                 "could not configure message lanes", false);
        remove_connection(callback.m_hConn);
        return;
      }
      if (!hosting) {
        host_connection = callback.m_hConn;
        if (!sockets->SetConnectionPollGroup(callback.m_hConn, poll_group)) {
          sockets->CloseConnection(callback.m_hConn, kCloseTransportSetup,
                                   "could not configure poll group", false);
          remove_connection(callback.m_hConn);
          return;
        }
      }
      if (!connection.connected) {
        connection.connected = true;
        events.push_back({.kind = TransportEventKind::CONNECTED,
                          .connection_id = connection.id,
                          .remote_address = connection.remote_address});
      }
      return;
    }
    if (state == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        state == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
      if (const auto found = gns_connections.find(callback.m_hConn);
          found != gns_connections.end()) {
        events.push_back({.kind = TransportEventKind::CLOSED,
                          .connection_id = found->second.id,
                          .close_reason = callback.m_info.m_eEndReason,
                          .detail = callback.m_info.m_szEndDebug,
                          .remote_address = found->second.remote_address});
        remove_connection(callback.m_hConn);
      }
      sockets->CloseConnection(callback.m_hConn, 0, nullptr, false);
    }
  }

  bool send_raw(const HSteamNetConnection connection,
                const std::span<const uint8_t> bytes,
                const TransportLane lane) const {
    if (bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      return false;
    }
    const auto delivery = lane_delivery(lane);
    const int flags =
        delivery == Delivery::RELIABLE_ORDERED
            ? k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle
            : k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_NoDelay;
    auto* message = SteamNetworkingUtils()->AllocateMessage(bytes.size());
    if (!message)
      return false;
    std::memcpy(message->m_pData, bytes.data(), bytes.size());
    message->m_cbSize = static_cast<int>(bytes.size());
    message->m_conn = connection;
    message->m_nFlags = flags;
    message->m_idxLane = static_cast<uint16>(lane);
    ISteamNetworkingMessage* messages[1] = {message};
    int64 result = 0;
    sockets->SendMessages(1, messages, &result, true);
    return result >= 0;
  }

  void receive_messages() {
    size_t received = 0;
    while (running && received < kMaximumTransportMessagesPerPump) {
      std::array<ISteamNetworkingMessage*, kReceiveBatchSize> messages = {};
      const auto remaining = kMaximumTransportMessagesPerPump - received;
      const int batch_size = static_cast<int>((std::min)(remaining, messages.size()));
      const int count =
          sockets->ReceiveMessagesOnPollGroup(poll_group, messages.data(), batch_size);
      if (count <= 0)
        return;
      received += static_cast<size_t>(count);
      for (int index = 0; index < count; ++index) {
        auto* message = messages[index];
        if (const auto found = gns_connections.find(message->m_conn);
            found != gns_connections.end() && found->second.connected && message->m_cbSize > 0 &&
            valid_lane(message->m_idxLane)) {
          const auto* begin = static_cast<const uint8_t*>(message->m_pData);
          const auto lane = static_cast<TransportLane>(message->m_idxLane);
          events.push_back({.kind = TransportEventKind::MESSAGE,
                            .connection_id = found->second.id,
                            .delivery = message_delivery(message->m_nFlags),
                            .lane = lane,
                            .remote_address = found->second.remote_address,
                            .payload = std::vector(begin, begin + message->m_cbSize)});
        }
        message->Release();
      }
    }
  }

  void drain_reliable_messages() {
    for (const auto& [handle, connection] : gns_connections) {
      if (connection.connected)
        sockets->FlushMessagesOnConnection(handle);
    }
    const auto deadline = std::chrono::steady_clock::now() + kGracefulShutdownTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
      bool drained = true;
      for (const auto& [handle, connection] : gns_connections) {
        if (!connection.connected)
          continue;
        SteamNetConnectionRealTimeStatus_t status = {};
        if (sockets->GetConnectionRealTimeStatus(handle, &status, 0, nullptr) == k_EResultOK &&
            (status.m_cbPendingReliable != 0 || status.m_cbSentUnackedReliable != 0)) {
          drained = false;
          break;
        }
      }
      if (drained)
        return;
      sockets->RunCallbacks();
      std::this_thread::yield();
    }
  }

  void shutdown(const int reason, const bool graceful = false) {
    if (!sockets)
      return;
    if (graceful)
      drain_reliable_messages();
    for (const auto& [handle, connection] : gns_connections) {
      sockets->CloseConnection(handle, reason, "transport shutdown", graceful);
    }
    sockets->RunCallbacks();
    gns_connections.clear();
    connection_handles.clear();
    if (listen_socket != k_HSteamListenSocket_Invalid)
      sockets->CloseListenSocket(listen_socket);
    if (poll_group != k_HSteamNetPollGroup_Invalid)
      sockets->DestroyPollGroup(poll_group);
    listen_socket = k_HSteamListenSocket_Invalid;
    poll_group = k_HSteamNetPollGroup_Invalid;
    host_connection = k_HSteamNetConnection_Invalid;
    hosting = false;
    running = false;
    events.clear();
  }
};

SessionPlatform::SessionPlatform() : impl_(std::make_unique<Impl>()) {}
SessionPlatform::~SessionPlatform() = default;

bool SessionPlatform::host(const TransportHostConfig& config) const {
  impl_->shutdown(0);
  if (!impl_->sockets) {
    lg::error("[GNS-Transport] Host failed: GNS sockets are unavailable.");
    return false;
  }
  if (config.maximum_pending_connections == 0) {
    lg::error("[GNS-Transport] Host failed: maximum pending connections is zero.");
    return false;
  }
  impl_->hosting = true;
  impl_->maximum_pending_connections = config.maximum_pending_connections;
  impl_->poll_group = impl_->sockets->CreatePollGroup();
  if (impl_->poll_group == k_HSteamNetPollGroup_Invalid) {
    lg::error("[GNS-Transport] Host failed: could not create the poll group.");
    return false;
  }
  SteamNetworkingIPAddr address;
  address.Clear();
  address.m_port = config.port;
  SteamNetworkingConfigValue_t option;
  option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                reinterpret_cast<void*>(Impl::connection_callback));
  impl_->listen_socket = impl_->sockets->CreateListenSocketIP(address, 1, &option);
  if (impl_->listen_socket == k_HSteamListenSocket_Invalid) {
    lg::error("[GNS-Transport] Host failed: could not bind UDP port {}.", config.port);
    impl_->shutdown(0);
    return false;
  }
  impl_->running = true;
  return true;
}

bool SessionPlatform::connect(const TransportClientConfig& config) const {
  impl_->shutdown(0);
  if (!impl_->sockets) {
    lg::error("[GNS-Transport] Connect failed: GNS sockets are unavailable.");
    return false;
  }
  if (config.endpoint.empty()) {
    lg::error("[GNS-Transport] Connect failed: endpoint is empty.");
    return false;
  }
  if (config.port == 0) {
    lg::error("[GNS-Transport] Connect failed: port is zero.");
    return false;
  }
  SteamNetworkingIPAddr address;
  address.Clear();
  if (!address.ParseString(config.endpoint.c_str())) {
    lg::error("[GNS-Transport] Connect failed: '{}' is not an IP address.", config.endpoint);
    return false;
  }
  address.m_port = config.port;
  impl_->hosting = false;
  impl_->poll_group = impl_->sockets->CreatePollGroup();
  if (impl_->poll_group == k_HSteamNetPollGroup_Invalid) {
    lg::error("[GNS-Transport] Connect failed: could not create the poll group.");
    return false;
  }
  SteamNetworkingConfigValue_t option;
  option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                reinterpret_cast<void*>(Impl::connection_callback));
  impl_->host_connection = impl_->sockets->ConnectByIPAddress(address, 1, &option);
  if (impl_->host_connection == k_HSteamNetConnection_Invalid) {
    lg::error("[GNS-Transport] ConnectByIPAddress failed for {}:{}.", config.endpoint, config.port);
    impl_->shutdown(0);
    return false;
  }
  auto& connection = impl_->add_connection(impl_->host_connection);
  connection.lanes_configured = impl_->configure_connection(impl_->host_connection);
  impl_->running = true;
  return true;
}

void SessionPlatform::shutdown(const int reason, const bool graceful) const {
  impl_->shutdown(reason, graceful);
}

void SessionPlatform::pump() const {
  if (!impl_->running)
    return;
  impl_->sockets->RunCallbacks();
  impl_->receive_messages();
  for (const auto& snapshot : connection_snapshots()) {
    if (snapshot.pending_reliable_bytes + snapshot.sent_unacked_reliable_bytes >
        kReliableBacklogLimitBytes) {
      close_connection(snapshot.connection_id, kCloseReliableBacklog, "reliable backlog saturated");
    }
  }
}

bool SessionPlatform::poll_event(TransportEvent& event) const {
  if (impl_->events.empty())
    return false;
  event = std::move(impl_->events.front());
  impl_->events.pop_front();
  return true;
}

bool SessionPlatform::send(const ConnectionId connection,
                           const std::span<const uint8_t> payload,
                           const Delivery delivery) const {
  return send(connection, payload,
              delivery == Delivery::RELIABLE_ORDERED ? TransportLane::GAMEPLAY_RELIABLE
                                                     : TransportLane::REALTIME_NORMAL);
}

bool SessionPlatform::send(const ConnectionId connection,
                           const std::span<const uint8_t> payload,
                           const TransportLane lane) const {
  const auto found = impl_->connection_handles.find(connection);
  if (!impl_->running || found == impl_->connection_handles.end())
    return false;
  const auto record = impl_->gns_connections.find(found->second);
  return record != impl_->gns_connections.end() && record->second.connected &&
         impl_->send_raw(found->second, payload, lane);
}

void SessionPlatform::close_connection(const ConnectionId connection,
                                       const int reason,
                                       const std::string_view detail) const {
  if (const auto found = impl_->connection_handles.find(connection);
      found != impl_->connection_handles.end()) {
    const auto handle = found->second;
    const auto record = impl_->gns_connections.find(handle);
    if (record == impl_->gns_connections.end())
      return;
    const std::string text(detail);
    impl_->events.push_back({.kind = TransportEventKind::CLOSED,
                             .connection_id = connection,
                             .close_reason = reason,
                             .detail = text,
                             .remote_address = record->second.remote_address});
    impl_->sockets->CloseConnection(handle, reason, text.c_str(), true);
    if (impl_->host_connection == handle)
      impl_->host_connection = k_HSteamNetConnection_Invalid;
    impl_->remove_connection(handle);
  }
}

bool SessionPlatform::active() const {
  return impl_->running;
}
bool SessionPlatform::is_host() const {
  return impl_->hosting;
}

uint16_t SessionPlatform::local_port() const {
  if (!impl_->running || impl_->listen_socket == k_HSteamListenSocket_Invalid)
    return 0;
  SteamNetworkingIPAddr address;
  return impl_->sockets->GetListenSocketAddress(impl_->listen_socket, &address) ? address.m_port
                                                                                : 0;
}

ConnectionId SessionPlatform::host_connection_id() const {
  const auto found = impl_->gns_connections.find(impl_->host_connection);
  return found == impl_->gns_connections.end() ? 0 : found->second.id;
}

std::vector<ConnectionSnapshot> SessionPlatform::connection_snapshots() const {
  std::vector<ConnectionSnapshot> result;
  for (const auto& [handle, connection] : impl_->gns_connections) {
    if (!connection.connected || !connection.lanes_configured)
      continue;
    SteamNetConnectionRealTimeStatus_t status = {};
    SteamNetConnectionRealTimeLaneStatus_t lanes[kLaneCount] = {};
    if (impl_->sockets->GetConnectionRealTimeStatus(handle, &status, kLaneCount, lanes) !=
            k_EResultOK ||
        status.m_eState != k_ESteamNetworkingConnectionState_Connected) {
      continue;
    }
    result.push_back(
        {.connection_id = connection.id,
         .ping_ms = status.m_nPing,
         .jitter_us = status.m_usecMaxJitter,
         .local_quality = status.m_flConnectionQualityLocal,
         .remote_quality = status.m_flConnectionQualityRemote,
         .send_bytes_per_second = status.m_flOutBytesPerSec,
         .receive_bytes_per_second = status.m_flInBytesPerSec,
         .send_rate_bytes_per_second = status.m_nSendRateBytesPerSecond,
         .pending_unreliable_bytes = status.m_cbPendingUnreliable,
         .pending_reliable_bytes = status.m_cbPendingReliable,
         .sent_unacked_reliable_bytes = status.m_cbSentUnackedReliable,
         .critical_queue_time_us = static_cast<int>(
             lanes[static_cast<size_t>(TransportLane::REALTIME_CRITICAL)].m_usecQueueTime),
         .normal_queue_time_us = static_cast<int>(
             lanes[static_cast<size_t>(TransportLane::REALTIME_NORMAL)].m_usecQueueTime),
         .bulk_queue_time_us = static_cast<int>(
             lanes[static_cast<size_t>(TransportLane::REALTIME_BULK)].m_usecQueueTime)});
  }
  return result;
}

AggregateSnapshot SessionPlatform::aggregate_snapshot() const {
  const auto connections = connection_snapshots();
  return aggregate_connection_snapshots(connections);
}

uint64_t transport_error_count() {
  return g_transport_error_count.load();
}
}  // namespace multiplayer::platform
