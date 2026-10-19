#include "game/multiplayer/platform/discovery/discovery_service.h"

#include <algorithm>
#include <vector>

#include "common/cross_sockets/XSocket.h"
#include "common/log/log.h"

#include "game/multiplayer/platform/discovery/invite.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <iphlpapi.h>

#include <WS2tcpip.h>
#include <WinSock2.h>
#else
#include <ifaddrs.h>

#include <net/if.h>
#endif

namespace multiplayer::platform {
namespace {
constexpr int kDirectedAttempts = 10;
constexpr int kBroadcastAttempts = 4;

struct LocalSubnet {
  uint32_t address = 0;
  uint32_t mask = 0;
};

std::vector<LocalSubnet> local_subnets() {
  std::vector<LocalSubnet> subnets;
#ifdef _WIN32
  ULONG size = 15 * 1024;
  std::vector<uint8_t> buffer(size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  ULONG result = GetAdaptersAddresses(
      AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
      adapters, &size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    result = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, adapters, &size);
  }
  if (result != NO_ERROR)
    return subnets;
  for (const auto* adapter = adapters; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
      continue;
    for (const auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
      if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET ||
          unicast->OnLinkPrefixLength > 32)
        continue;
      const auto* local = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
      subnets.push_back({.address = ntohl(local->sin_addr.s_addr),
                         .mask = unicast->OnLinkPrefixLength == 0
                                     ? 0
                                     : 0xffffffffu << (32 - unicast->OnLinkPrefixLength)});
    }
  }
#else
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0)
    return subnets;
  for (const auto* interface = interfaces; interface; interface = interface->ifa_next) {
    if (!(interface->ifa_flags & IFF_UP) || !(interface->ifa_flags & IFF_BROADCAST) ||
        (interface->ifa_flags & IFF_LOOPBACK) || !interface->ifa_addr ||
        interface->ifa_addr->sa_family != AF_INET || !interface->ifa_netmask ||
        interface->ifa_netmask->sa_family != AF_INET)
      continue;
    const auto* local = reinterpret_cast<const sockaddr_in*>(interface->ifa_addr);
    const auto* netmask = reinterpret_cast<const sockaddr_in*>(interface->ifa_netmask);
    subnets.push_back(
        {.address = ntohl(local->sin_addr.s_addr), .mask = ntohl(netmask->sin_addr.s_addr)});
  }
  freeifaddrs(interfaces);
#endif
  return subnets;
}

std::vector<sockaddr_in> broadcast_targets(const uint16_t port) {
  std::vector<sockaddr_in> targets;
  const auto add_target = [&](const uint32_t address) {
    if (std::ranges::any_of(
            targets, [&](const auto& existing) { return existing.sin_addr.s_addr == address; }))
      return;
    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    target.sin_addr.s_addr = address;
    targets.push_back(target);
  };
  add_target(INADDR_BROADCAST);
  for (const auto& subnet : local_subnets())
    add_target(htonl((subnet.address & subnet.mask) | ~subnet.mask));
  return targets;
}
}  // namespace

bool is_lan_address(const std::string& address) {
  if (in_addr v4{}; inet_pton(AF_INET, address.c_str(), &v4) == 1) {
    const uint32_t ip = ntohl(v4.s_addr);
    return (ip & 0xff000000) == 0x7f000000 || (ip & 0xff000000) == 0x0a000000 ||
           (ip & 0xfff00000) == 0xac100000 || (ip & 0xffff0000) == 0xc0a80000 ||
           (ip & 0xffff0000) == 0xa9fe0000 ||
           std::ranges::any_of(local_subnets(), [&](const auto& subnet) {
             return subnet.mask != 0 && (ip & subnet.mask) == (subnet.address & subnet.mask);
           });
  }
  in6_addr v6{};
  if (inet_pton(AF_INET6, address.c_str(), &v6) != 1)
    return false;
  const auto* bytes = v6.s6_addr;
  const bool loopback =
      std::all_of(bytes, bytes + 15, [](auto b) { return b == 0; }) && bytes[15] == 1;
  return loopback || (bytes[0] & 0xfe) == 0xfc || (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80);
}

bool discovery_advertisement_matches(const DiscoveryConfig& config,
                                     const DiscoveryAdvertisement& advertisement,
                                     const std::string_view source_address) {
  return advertisement.game_id == config.game_id &&
         (config.directed_address.empty() ||
          (source_address == config.directed_address &&
           advertisement.game_port == config.expected_game_port)) &&
         (config.include_full_sessions ||
          advertisement.current_players < advertisement.player_limit);
}

DiscoveryScanner::~DiscoveryScanner() {
  stop();
}

bool DiscoveryScanner::start(DiscoveryConfig config) {
  stop();
  if (config.discovery_port == 0) {
    lg::error("[Discovery] Scanner start failed: discovery_port is 0.");
    return false;
  }
  if (config.game_id.empty()) {
    lg::error("[Discovery] Scanner start failed: game_id is empty.");
    return false;
  }
  if (!config.directed_address.empty()) {
    sockaddr_in address = {};
    if (inet_pton(AF_INET, config.directed_address.c_str(), &address.sin_addr) != 1) {
      lg::error("[Discovery] Scanner start failed: invalid directed address '{}'.",
                config.directed_address);
      return false;
    }
    if (config.expected_game_port == 0) {
      lg::error("[Discovery] Scanner start failed: expected_game_port is 0.");
      return false;
    }
  }
  stopping_ = false;
  status_ = DiscoveryStatus::SEARCHING;
  {
    std::lock_guard lock(result_mutex_);
    results_.clear();
  }
  thread_ = std::thread([this, config = std::move(config)]() mutable { scan(std::move(config)); });
  return true;
}

void DiscoveryScanner::stop() {
  stopping_ = true;
  if (thread_.joinable())
    thread_.join();
  status_ = DiscoveryStatus::IDLE;
  std::lock_guard lock(result_mutex_);
  results_.clear();
}

std::vector<DiscoveryResult> DiscoveryScanner::results() {
  std::lock_guard lock(result_mutex_);
  return results_;
}

bool DiscoveryScanner::add_result(DiscoveryResult result) {
  std::lock_guard lock(result_mutex_);
  const auto existing = std::ranges::find_if(results_, [&](const DiscoveryResult& found) {
    return found.advertisement.room_code == result.advertisement.room_code;
  });
  if (existing != results_.end()) {
    existing->advertisement = std::move(result.advertisement);
    return false;
  }
  results_.push_back(std::move(result));
  return true;
}

void DiscoveryScanner::scan(DiscoveryConfig config) {
  const int socket = open_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket < 0) {
    lg::error("[Discovery] Failed to open UDP socket for discovery scanning.");
    status_ = DiscoveryStatus::FAILED;
    return;
  }
  int broadcast = 1;
  set_socket_option(socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
  set_socket_timeout(socket, 500000);
  std::vector<sockaddr_in> targets;
  if (config.directed_address.empty()) {
    targets = broadcast_targets(config.discovery_port);
  } else {
    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(config.discovery_port);
    inet_pton(AF_INET, config.directed_address.c_str(), &target.sin_addr);
    targets.push_back(target);
  }
  const auto query = encode_discovery_query(config.game_id);
  const bool directed = !config.directed_address.empty();
  const int attempts = directed ? kDirectedAttempts : kBroadcastAttempts;
  for (int attempt = 0; attempt < attempts && !stopping_; ++attempt) {
    for (const auto& target : targets) {
      sendto(socket, reinterpret_cast<const char*>(query.data()), query.size(), 0,
             reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }
    while (!stopping_) {
      uint8_t buffer[512] = {};
      sockaddr_in source = {};
      socklen_t source_size = sizeof(source);
      const int received = recvfrom(socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                    reinterpret_cast<sockaddr*>(&source), &source_size);
      if (received <= 0) {
        if (received < 0 && !stopping_)
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
        break;
      }
      DiscoveryAdvertisement advertisement;
      if (ntohs(source.sin_port) != config.discovery_port ||
          !decode_discovery_advertisement({buffer, static_cast<size_t>(received)}, advertisement)) {
        continue;
      }
      const std::string address = address_to_string(source);
      if (!discovery_advertisement_matches(config, advertisement, address))
        continue;
      const uint16_t game_port = advertisement.game_port;
      const std::string room_code = advertisement.room_code;
      if (add_result({.invite = make_invite(address, game_port, room_code),
                      .advertisement = std::move(advertisement)})) {
        lg::info("[Discovery] Discovered host at {}:{} (room: {}).", address, game_port, room_code);
      }
      if (directed) {
        status_ = DiscoveryStatus::FOUND;
        close_socket(socket);
        return;
      }
    }
  }
  if (!stopping_) {
    if (!results().empty())
      status_ = DiscoveryStatus::FOUND;
    else if (directed) {
      status_ = DiscoveryStatus::TIMED_OUT;
      lg::warn(
          "[Discovery] Directed discovery timed out for {}:{} (no response on discovery port {}).",
          config.directed_address, config.expected_game_port, config.discovery_port);
    } else {
      status_ = DiscoveryStatus::TIMED_OUT;
      lg::info("[Discovery] No LAN hosts responded on discovery port {}.", config.discovery_port);
    }
  }
  close_socket(socket);
}

DiscoveryResponder::~DiscoveryResponder() {
  stop();
}

bool DiscoveryResponder::start(const uint16_t discovery_port,
                               DiscoveryAdvertisement advertisement) {
  stop();
  if (discovery_port == 0) {
    lg::error("[Discovery] Responder start failed: discovery_port is 0.");
    return false;
  }
  if (const auto encoded = encode_discovery_advertisement(advertisement); encoded.empty()) {
    lg::error(
        "[Discovery] Responder start failed: encode_discovery_advertisement failed (game_port={}, "
        "current_players={}, player_limit={}, room='{}', game='{}').",
        advertisement.game_port, advertisement.current_players, advertisement.player_limit,
        advertisement.room_code, advertisement.game_id);
    return false;
  }
  discovery_port_ = discovery_port;
  {
    std::lock_guard lock(advertisement_mutex_);
    advertisement_ = std::move(advertisement);
  }
  active_ = true;
  thread_ = std::thread([this, discovery_port] { respond(discovery_port); });
  return true;
}

void DiscoveryResponder::update(DiscoveryAdvertisement advertisement) {
  if (encode_discovery_advertisement(advertisement).empty())
    return;
  std::lock_guard lock(advertisement_mutex_);
  advertisement_ = std::move(advertisement);
}

void DiscoveryResponder::stop() {
  active_ = false;
  if (thread_.joinable()) {
    if (const int socket = open_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); socket >= 0) {
      sockaddr_in loopback = {};
      loopback.sin_family = AF_INET;
      loopback.sin_port = htons(discovery_port_);
      loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      sendto(socket, "", 0, 0, reinterpret_cast<sockaddr*>(&loopback), sizeof(loopback));
      close_socket(socket);
    }
    thread_.join();
  }
  discovery_port_ = 0;
}

void DiscoveryResponder::respond(const uint16_t discovery_port) {
  const int socket = open_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket < 0) {
    lg::error("[Discovery] Responder failed: could not open UDP socket on port {}.",
              discovery_port);
    active_ = false;
    return;
  }
  sockaddr_in listen = {};
  listen.sin_family = AF_INET;
  listen.sin_port = htons(discovery_port);
  listen.sin_addr.s_addr = INADDR_ANY;
  if (bind(socket, reinterpret_cast<sockaddr*>(&listen), sizeof(listen)) < 0) {
    lg::error(
        "[Discovery] Responder failed: could not bind UDP discovery port {} (port may already be "
        "in "
        "use).",
        discovery_port);
    active_ = false;
    close_socket(socket);
    return;
  }
  lg::info("[Discovery] Discovery responder listening on UDP port {}.", discovery_port);
  set_socket_timeout(socket, 200000);
  uint8_t buffer[512] = {};
  while (active_) {
    sockaddr_in source = {};
    socklen_t source_size = sizeof(source);
    const int received = recvfrom(socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                  reinterpret_cast<sockaddr*>(&source), &source_size);
    if (received <= 0)
      continue;
    std::string requested_game;
    if (!listed_ ||
        !decode_discovery_query({buffer, static_cast<size_t>(received)}, requested_game))
      continue;
    DiscoveryAdvertisement advertisement;
    {
      std::lock_guard lock(advertisement_mutex_);
      advertisement = advertisement_;
    }
    if (requested_game != advertisement.game_id)
      continue;
    const auto reply = encode_discovery_advertisement(advertisement);
    sendto(socket, reinterpret_cast<const char*>(reply.data()), reply.size(), 0,
           reinterpret_cast<sockaddr*>(&source), source_size);
  }
  close_socket(socket);
}

}  // namespace multiplayer::platform
