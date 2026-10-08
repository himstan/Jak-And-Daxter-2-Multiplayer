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
std::vector<sockaddr_in> broadcast_targets(const uint16_t port) {
  std::vector<sockaddr_in> targets;
  sockaddr_in global = {};
  global.sin_family = AF_INET;
  global.sin_port = htons(port);
  global.sin_addr.s_addr = INADDR_BROADCAST;
  targets.push_back(global);
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
    return targets;
  for (const auto* adapter = adapters; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
      continue;
    for (const auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
      if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET ||
          unicast->OnLinkPrefixLength > 32)
        continue;
      const auto* local = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
      const uint32_t ip = ntohl(local->sin_addr.s_addr);
      const uint32_t mask =
          unicast->OnLinkPrefixLength == 0 ? 0 : 0xffffffffu << (32 - unicast->OnLinkPrefixLength);
      add_target(htonl((ip & mask) | ~mask));
    }
  }
#else
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0)
    return targets;
  for (const auto* interface = interfaces; interface; interface = interface->ifa_next) {
    if (!(interface->ifa_flags & IFF_UP) || !(interface->ifa_flags & IFF_BROADCAST) ||
        (interface->ifa_flags & IFF_LOOPBACK) || !interface->ifa_addr ||
        interface->ifa_addr->sa_family != AF_INET || !interface->ifa_broadaddr ||
        interface->ifa_broadaddr->sa_family != AF_INET)
      continue;
    const auto* broadcast = reinterpret_cast<const sockaddr_in*>(interface->ifa_broadaddr);
    add_target(broadcast->sin_addr.s_addr);
  }
  freeifaddrs(interfaces);
#endif
  return targets;
}
}  // namespace

bool discovery_advertisement_matches(const DiscoveryConfig& config,
                                     const DiscoveryAdvertisement& advertisement,
                                     const std::string_view source_address) {
  return advertisement.game_id == config.game_id &&
         advertisement.compatibility_identity == config.compatibility_identity &&
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
  if (config.compatibility_identity.empty()) {
    lg::error("[Discovery] Scanner start failed: compatibility_identity is empty.");
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
    result_.reset();
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
  result_.reset();
}

std::optional<DiscoveryResult> DiscoveryScanner::take_result() {
  std::lock_guard lock(result_mutex_);
  auto result = std::move(result_);
  result_.reset();
  if (status_ == DiscoveryStatus::FOUND) {
    status_ = DiscoveryStatus::IDLE;
  }
  return result;
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
  for (int attempt = 0; attempt < 10 && !stopping_; ++attempt) {
    for (const auto& target : targets) {
      sendto(socket, reinterpret_cast<const char*>(query.data()), query.size(), 0,
             reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }
    uint8_t buffer[512] = {};
    sockaddr_in source = {};
    socklen_t source_size = sizeof(source);
    const int received = recvfrom(socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                  reinterpret_cast<sockaddr*>(&source), &source_size);
    if (received <= 0) {
      if (received < 0 && !stopping_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
      continue;
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
    {
      DiscoveryResult result = {.invite = make_invite(address, game_port, room_code),
                                .advertisement = std::move(advertisement)};
      std::lock_guard lock(result_mutex_);
      result_ = std::move(result);
    }
    status_ = DiscoveryStatus::FOUND;
    lg::info("[Discovery] Discovered host at {}:{} (room: {}).", address, game_port, room_code);
    close_socket(socket);
    return;
  }
  if (!stopping_) {
    status_ = DiscoveryStatus::TIMED_OUT;
    if (!config.directed_address.empty()) {
      lg::warn(
          "[Discovery] Directed discovery timed out for {}:{} (no response on discovery port {}).",
          config.directed_address, config.expected_game_port, config.discovery_port);
    } else {
      lg::warn("[Discovery] LAN discovery timed out (no hosts responded on discovery port {}).",
               config.discovery_port);
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
    if (!decode_discovery_query({buffer, static_cast<size_t>(received)}, requested_game))
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
