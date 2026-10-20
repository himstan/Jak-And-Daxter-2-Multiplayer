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

#include "game/external/steam/steam_client.h"
#include "game/multiplayer/platform/core/limits.h"
#ifdef ENABLE_NETWORK_SIMULATION
#include "game/multiplayer/platform/transport/network_simulation.h"
#endif
#include "steam/isteamnetworkingutils.h"
#include "steam/steamnetworkingsockets.h"

namespace multiplayer::platform {
namespace {
constexpr int kCloseTransportSetup = 4000;
constexpr int kCloseReliableBacklog = 4001;
constexpr auto kGracefulShutdownTimeout = std::chrono::milliseconds(500);
constexpr int kLaneCount = 5;

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

constexpr size_t kDirectBackend = 0;
constexpr size_t kSteamBackend = 1;

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
    uint64_t steam_id = 0;
  };

  struct Backend {
    ISteamNetworkingSockets* sockets = nullptr;
    ISteamNetworkingUtils* utils = nullptr;
    HSteamListenSocket listen_socket = k_HSteamListenSocket_Invalid;
    bool open = false;
    std::unordered_map<HSteamNetConnection, Connection> connections;

    bool active() const { return sockets && open; }

    bool owns(const SteamNetConnectionStatusChangedCallback_t& info) const {
      return connections.contains(info.m_hConn) || (listen_socket != k_HSteamListenSocket_Invalid &&
                                                    info.m_info.m_hListenSocket == listen_socket);
    }

    void close(const int reason, const bool graceful) {
      if (!active())
        return;
      for (const auto& [handle, connection] : connections)
        sockets->CloseConnection(handle, reason, "transport shutdown", graceful);
      sockets->RunCallbacks();
      connections.clear();
      if (listen_socket != k_HSteamListenSocket_Invalid)
        sockets->CloseListenSocket(listen_socket);
      listen_socket = k_HSteamListenSocket_Invalid;
      open = false;
    }
  };

  struct Handle {
    Backend* backend = nullptr;
    HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
  };

  std::array<Backend, 2> backends;
  ConnectionId host_connection = 0;
  bool hosting = false;
  bool running = false;
  size_t maximum_pending_connections = 16;
  ConnectionId next_connection_id = 1;
  size_t next_receive_source = 0;
  std::unordered_map<ConnectionId, Handle> connection_handles;
  std::deque<TransportEvent> events;

  Impl() {
    if (acquire_gns()) {
      backends[kDirectBackend].sockets = SteamNetworkingSockets();
      backends[kDirectBackend].utils = SteamNetworkingUtils();
    }
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

  template <size_t BackendIndex>
  static void connection_callback(const SteamNetConnectionStatusChangedCallback_t* info) {
    std::lock_guard lock(g_init_mutex);
    for (auto* instance : g_instances) {
      if (auto& backend = instance->backends[BackendIndex]; backend.owns(*info)) {
        instance->on_connection_changed(backend, *info);
        return;
      }
    }
  }

  static SteamNetworkingConfigValue_t callback_option(const size_t backend) {
    SteamNetworkingConfigValue_t option;
    option.SetPtr(
        k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
        reinterpret_cast<void*>(backend == kSteamBackend ? connection_callback<kSteamBackend>
                                                         : connection_callback<kDirectBackend>));
    return option;
  }

  static std::string remote_string(const SteamNetConnectionInfo_t& info) {
    if (info.m_identityRemote.GetSteamID64() != 0) {
      char text[SteamNetworkingIdentity::k_cchMaxString] = {};
      info.m_identityRemote.ToString(text, sizeof(text));
      return text;
    }
    char text[SteamNetworkingIPAddr::k_cchMaxString] = {};
    info.m_addrRemote.ToString(text, sizeof(text), false);
    return text;
  }

  bool use_steam() {
    const auto networking = steam::networking();
    auto& steam = backends[kSteamBackend];
    steam.sockets = networking ? networking->sockets : nullptr;
    steam.utils = networking ? networking->utils : nullptr;
#ifdef ENABLE_NETWORK_SIMULATION
    if (networking)
      apply_steam_network_simulation(*networking->utils);
#endif
    return networking.has_value();
  }

  static bool configure_connection(const Backend& backend, const HSteamNetConnection connection) {
    const int priorities[kLaneCount] = {0, 0, 1, 1, 2};
    const uint16 weights[kLaneCount] = {1, 4, 1, 1, 1};
    return backend.sockets->ConfigureConnectionLanes(connection, kLaneCount, priorities, weights) ==
           k_EResultOK;
  }

  Connection& add_connection(Backend& backend,
                             const HSteamNetConnection connection,
                             const SteamNetConnectionInfo_t* remote = nullptr) {
    auto [found, inserted] = backend.connections.try_emplace(connection);
    if (inserted) {
      found->second.connection = connection;
      found->second.id = next_connection_id++;
      connection_handles[found->second.id] = {&backend, connection};
    }
    if (remote) {
      found->second.remote_address = remote_string(*remote);
      if (&backend == &backends[kSteamBackend])
        found->second.steam_id = remote->m_identityRemote.GetSteamID64();
    }
    return found->second;
  }

  void remove_connection(Backend& backend, const HSteamNetConnection connection) {
    const auto found = backend.connections.find(connection);
    if (found == backend.connections.end())
      return;
    connection_handles.erase(found->second.id);
    backend.connections.erase(found);
  }

  void on_connection_changed(Backend& backend,
                             const SteamNetConnectionStatusChangedCallback_t& callback) {
    auto* sockets = backend.sockets;
    const auto state = callback.m_info.m_eState;
    if (state == k_ESteamNetworkingConnectionState_Connecting && hosting) {
      if (!backend.connections.contains(callback.m_hConn) &&
          connection_handles.size() >= maximum_pending_connections) {
        sockets->CloseConnection(callback.m_hConn, kCloseTransportSetup,
                                 "transport capacity exceeded", false);
        return;
      }
      auto& connection = add_connection(backend, callback.m_hConn, &callback.m_info);
      connection.lanes_configured = configure_connection(backend, callback.m_hConn);
      if (!connection.lanes_configured ||
          sockets->AcceptConnection(callback.m_hConn) != k_EResultOK) {
        sockets->CloseConnection(callback.m_hConn, kCloseTransportSetup, "transport setup failed",
                                 false);
        remove_connection(backend, callback.m_hConn);
      }
      return;
    }
    if (state == k_ESteamNetworkingConnectionState_Connected) {
      auto& connection = add_connection(backend, callback.m_hConn, &callback.m_info);
      if (!((connection.lanes_configured = configure_connection(backend, callback.m_hConn)))) {
        sockets->CloseConnection(callback.m_hConn, kCloseTransportSetup,
                                 "could not configure message lanes", false);
        remove_connection(backend, callback.m_hConn);
        return;
      }
      if (!hosting)
        host_connection = connection.id;
      if (!connection.connected) {
        connection.connected = true;
        events.push_back({.kind = TransportEventKind::CONNECTED,
                          .connection_id = connection.id,
                          .remote_address = connection.remote_address,
                          .steam_id = connection.steam_id});
      }
      return;
    }
    if (state == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        state == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
      if (const auto found = backend.connections.find(callback.m_hConn);
          found != backend.connections.end()) {
        drain_messages(backend, found->second);
        events.push_back(
            {.kind = TransportEventKind::CLOSED,
             .connection_id = found->second.id,
             .close_reason = callback.m_info.m_eEndReason,
             .timed_out = callback.m_info.m_eEndReason == k_ESteamNetConnectionEnd_Remote_Timeout ||
                          callback.m_info.m_eEndReason == k_ESteamNetConnectionEnd_Misc_Timeout,
             .detail = callback.m_info.m_szEndDebug,
             .remote_address = found->second.remote_address});
        remove_connection(backend, callback.m_hConn);
      }
      sockets->CloseConnection(callback.m_hConn, 0, nullptr, false);
    }
  }

  static bool send_raw(const Backend& backend,
                       const HSteamNetConnection connection,
                       const std::span<const uint8_t> bytes,
                       const TransportLane lane) {
    if (bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      return false;
    }
    const auto delivery = lane_delivery(lane);
    const int flags =
        delivery == Delivery::RELIABLE_ORDERED
            ? k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle
            : k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_NoDelay;
    auto* message = backend.utils->AllocateMessage(bytes.size());
    if (!message)
      return false;
    std::memcpy(message->m_pData, bytes.data(), bytes.size());
    message->m_cbSize = static_cast<int>(bytes.size());
    message->m_conn = connection;
    message->m_nFlags = flags;
    message->m_idxLane = static_cast<uint16>(lane);
    ISteamNetworkingMessage* messages[1] = {message};
    int64 result = 0;
    backend.sockets->SendMessages(1, messages, &result, true);
    return result >= 0;
  }

  void run_callbacks() {
    for (const auto& backend : backends) {
      if (backend.active())
        backend.sockets->RunCallbacks();
    }
  }

  void receive_messages() {
    std::vector<std::pair<const Backend*, const Connection*>> sources;
    for (const auto& backend : backends) {
      for (const auto& [handle, connection] : backend.connections) {
        if (backend.active() && connection.connected)
          sources.emplace_back(&backend, &connection);
      }
    }
    size_t received = 0;
    for (size_t offset = 0; offset < sources.size() && received < kMaximumTransportMessagesPerPump;
         ++offset) {
      const auto& [backend, connection] = sources[(next_receive_source + offset) % sources.size()];
      std::array<ISteamNetworkingMessage*, kMaximumTransportMessagesPerConnectionPerPump> messages =
          {};
      const auto remaining = kMaximumTransportMessagesPerPump - received;
      const int count = backend->sockets->ReceiveMessagesOnConnection(
          connection->connection, messages.data(),
          static_cast<int>((std::min)(remaining, messages.size())));
      if (count <= 0)
        continue;
      received += static_cast<size_t>(count);
      push_messages(*connection, std::span(messages.data(), static_cast<size_t>(count)));
    }
    ++next_receive_source;
  }

  void push_messages(const Connection& connection,
                     const std::span<ISteamNetworkingMessage* const> messages) {
    for (auto* message : messages) {
      if (message->m_cbSize > 0 && valid_lane(message->m_idxLane)) {
        const auto* begin = static_cast<const uint8_t*>(message->m_pData);
        events.push_back({.kind = TransportEventKind::MESSAGE,
                          .connection_id = connection.id,
                          .delivery = message_delivery(message->m_nFlags),
                          .lane = static_cast<TransportLane>(message->m_idxLane),
                          .remote_address = connection.remote_address,
                          .payload = std::vector(begin, begin + message->m_cbSize)});
      }
      message->Release();
    }
  }

  void drain_messages(const Backend& backend, const Connection& connection) {
    std::array<ISteamNetworkingMessage*, kMaximumTransportMessagesPerConnectionPerPump> messages =
        {};
    int count = 0;
    while ((count = backend.sockets->ReceiveMessagesOnConnection(
                connection.connection, messages.data(), static_cast<int>(messages.size()))) > 0) {
      push_messages(connection, std::span(messages.data(), static_cast<size_t>(count)));
    }
  }

  bool reliable_messages_drained() const {
    for (const auto& backend : backends) {
      for (const auto& [handle, connection] : backend.connections) {
        SteamNetConnectionRealTimeStatus_t status = {};
        if (connection.connected &&
            backend.sockets->GetConnectionRealTimeStatus(handle, &status, 0, nullptr) ==
                k_EResultOK &&
            (status.m_cbPendingReliable != 0 || status.m_cbSentUnackedReliable != 0)) {
          return false;
        }
      }
    }
    return true;
  }

  void drain_reliable_messages() {
    for (const auto& backend : backends) {
      for (const auto& [handle, connection] : backend.connections) {
        if (connection.connected)
          backend.sockets->FlushMessagesOnConnection(handle);
      }
    }
    const auto deadline = std::chrono::steady_clock::now() + kGracefulShutdownTimeout;
    while (std::chrono::steady_clock::now() < deadline && !reliable_messages_drained()) {
      run_callbacks();
      std::this_thread::yield();
    }
  }

  void shutdown(const int reason, const bool graceful = false) {
    if (graceful)
      drain_reliable_messages();
    for (auto& backend : backends)
      backend.close(reason, graceful);
    connection_handles.clear();
    host_connection = 0;
    hosting = false;
    running = false;
    events.clear();
  }
};

SessionPlatform::SessionPlatform() : impl_(std::make_unique<Impl>()) {}
SessionPlatform::~SessionPlatform() = default;

bool SessionPlatform::host(const TransportHostConfig& config) const {
  impl_->shutdown(0);
  auto& direct = impl_->backends[kDirectBackend];
  auto& steam = impl_->backends[kSteamBackend];
  if (!direct.sockets || (config.steam && !impl_->use_steam())) {
    lg::error("[GNS-Transport] Host failed: {} sockets are unavailable.",
              direct.sockets ? "Steam" : "GNS");
    return false;
  }
  if (config.maximum_pending_connections == 0) {
    lg::error("[GNS-Transport] Host failed: maximum pending connections is zero.");
    return false;
  }
  impl_->hosting = true;
  impl_->maximum_pending_connections = config.maximum_pending_connections;
  direct.open = true;
  steam.open = config.steam;
  SteamNetworkingIPAddr address;
  address.Clear();
  address.m_port = config.port;
  auto direct_option = Impl::callback_option(kDirectBackend);
  direct.listen_socket = direct.sockets->CreateListenSocketIP(address, 1, &direct_option);
  if (config.steam && direct.listen_socket != k_HSteamListenSocket_Invalid) {
    auto steam_option = Impl::callback_option(kSteamBackend);
    steam.listen_socket = steam.sockets->CreateListenSocketP2P(0, 1, &steam_option);
  }
  if (direct.listen_socket == k_HSteamListenSocket_Invalid ||
      (config.steam && steam.listen_socket == k_HSteamListenSocket_Invalid)) {
    lg::error("[GNS-Transport] Host failed: could not open the {} listen socket.",
              direct.listen_socket == k_HSteamListenSocket_Invalid ? "UDP" : "Steam P2P");
    impl_->shutdown(0);
    return false;
  }
  impl_->running = true;
  return true;
}

bool SessionPlatform::connect(const TransportClientConfig& config) const {
  impl_->shutdown(0);
  const bool steam = config.steam_host != 0;
  const size_t backend_index = steam ? kSteamBackend : kDirectBackend;
  auto& backend = impl_->backends[backend_index];
  if (steam ? !impl_->use_steam() : !backend.sockets) {
    lg::error("[GNS-Transport] Connect failed: {} sockets are unavailable.",
              steam ? "Steam" : "GNS");
    return false;
  }
  SteamNetworkingIPAddr address;
  address.Clear();
  if (!steam) {
    if (config.endpoint.empty()) {
      lg::error("[GNS-Transport] Connect failed: endpoint is empty.");
      return false;
    }
    if (config.port == 0) {
      lg::error("[GNS-Transport] Connect failed: port is zero.");
      return false;
    }
    if (!address.ParseString(config.endpoint.c_str())) {
      lg::error("[GNS-Transport] Connect failed: '{}' is not an IP address.", config.endpoint);
      return false;
    }
    address.m_port = config.port;
  }
  impl_->hosting = false;
  backend.open = true;
  auto option = Impl::callback_option(backend_index);
  HSteamNetConnection handle = k_HSteamNetConnection_Invalid;
  if (steam) {
    SteamNetworkingIdentity identity;
    identity.SetSteamID64(config.steam_host);
    handle = backend.sockets->ConnectP2P(identity, 0, 1, &option);
  } else {
    handle = backend.sockets->ConnectByIPAddress(address, 1, &option);
  }
  if (handle == k_HSteamNetConnection_Invalid) {
    lg::error("[GNS-Transport] Connect failed for {}.",
              steam ? std::to_string(config.steam_host) : config.endpoint);
    impl_->shutdown(0);
    return false;
  }
  auto& connection = impl_->add_connection(backend, handle);
  connection.lanes_configured = Impl::configure_connection(backend, handle);
  impl_->host_connection = connection.id;
  impl_->running = true;
  return true;
}

void SessionPlatform::shutdown(const int reason, const bool graceful) const {
  impl_->shutdown(reason, graceful);
}

void SessionPlatform::pump() const {
  if (!impl_->running)
    return;
  impl_->run_callbacks();
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
  const auto& [backend, handle] = found->second;
  const auto record = backend->connections.find(handle);
  return record != backend->connections.end() && record->second.connected &&
         Impl::send_raw(*backend, handle, payload, lane);
}

void SessionPlatform::close_connection(const ConnectionId connection,
                                       const int reason,
                                       const std::string_view detail) const {
  const auto found = impl_->connection_handles.find(connection);
  if (found == impl_->connection_handles.end())
    return;
  const auto [backend, handle] = found->second;
  const auto record = backend->connections.find(handle);
  if (record == backend->connections.end())
    return;
  const std::string text(detail);
  impl_->events.push_back({.kind = TransportEventKind::CLOSED,
                           .connection_id = connection,
                           .close_reason = reason,
                           .detail = text,
                           .remote_address = record->second.remote_address});
  backend->sockets->CloseConnection(handle, reason, text.c_str(), true);
  if (impl_->host_connection == connection)
    impl_->host_connection = 0;
  impl_->remove_connection(*backend, handle);
}

bool SessionPlatform::active() const {
  return impl_->running;
}
bool SessionPlatform::is_host() const {
  return impl_->hosting;
}

uint16_t SessionPlatform::local_port() const {
  const auto& direct = impl_->backends[kDirectBackend];
  if (!impl_->running || direct.listen_socket == k_HSteamListenSocket_Invalid)
    return 0;
  SteamNetworkingIPAddr address;
  return direct.sockets->GetListenSocketAddress(direct.listen_socket, &address) ? address.m_port
                                                                                : 0;
}

ConnectionId SessionPlatform::host_connection_id() const {
  return impl_->connection_handles.contains(impl_->host_connection) ? impl_->host_connection : 0;
}

std::vector<ConnectionSnapshot> SessionPlatform::connection_snapshots() const {
  std::vector<ConnectionSnapshot> result;
  for (const auto& backend : impl_->backends) {
    for (const auto& [handle, connection] : backend.connections) {
      if (!connection.connected || !connection.lanes_configured)
        continue;
      SteamNetConnectionRealTimeStatus_t status = {};
      SteamNetConnectionRealTimeLaneStatus_t lanes[kLaneCount] = {};
      if (backend.sockets->GetConnectionRealTimeStatus(handle, &status, kLaneCount, lanes) !=
              k_EResultOK ||
          status.m_eState != k_ESteamNetworkingConnectionState_Connected) {
        continue;
      }
      SteamNetConnectionInfo_t info = {};
      const bool relayed = backend.sockets->GetConnectionInfo(handle, &info) &&
                           (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0;
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
               lanes[static_cast<size_t>(TransportLane::REALTIME_BULK)].m_usecQueueTime),
           .relayed = relayed});
    }
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
