#include "multiplayer_scanner.h"
#include "multiplayer_protocol.h"
#include "multiplayer_security.h"
#include "multiplayer_preferences.h"
#include "common/cross_sockets/XSocket.h"
#include "common/log/log.h"
#include <charconv>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#endif

namespace {
std::vector<sockaddr_in> get_lan_discovery_targets() {
  std::vector<sockaddr_in> targets;

  sockaddr_in global_broadcast = {};
  global_broadcast.sin_family = AF_INET;
  global_broadcast.sin_port = htons(DISCOVERY_PORT);
  global_broadcast.sin_addr.s_addr = INADDR_BROADCAST;
  targets.push_back(global_broadcast);

#ifdef _WIN32
  ULONG buffer_size = 15 * 1024;
  std::vector<uint8_t> adapter_buffer(buffer_size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapter_buffer.data());

  ULONG result = GetAdaptersAddresses(AF_INET,
                                      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, adapters, &buffer_size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    adapter_buffer.resize(buffer_size);
    adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapter_buffer.data());
    result = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, adapters, &buffer_size);
  }
  if (result != NO_ERROR) {
    lg::warn("[Multiplayer] Could not enumerate LAN adapters for directed broadcast: {}", result);
    return targets;
  }

  for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
      continue;
    }

    for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
      if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET ||
          unicast->OnLinkPrefixLength > 32) {
        continue;
      }

      const auto* local_addr = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
      const uint32_t local_ip = ntohl(local_addr->sin_addr.s_addr);
      const uint32_t mask = unicast->OnLinkPrefixLength == 0
                                ? 0
                                : (0xffffffffu << (32 - unicast->OnLinkPrefixLength));
      const uint32_t broadcast_ip = (local_ip & mask) | ~mask;

      sockaddr_in target = {};
      target.sin_family = AF_INET;
      target.sin_port = htons(DISCOVERY_PORT);
      target.sin_addr.s_addr = htonl(broadcast_ip);

      bool already_added = false;
      for (const auto& existing : targets) {
        if (existing.sin_addr.s_addr == target.sin_addr.s_addr) {
          already_added = true;
          break;
        }
      }
      if (!already_added) {
        targets.push_back(target);
      }
    }
  }
#endif

  return targets;
}
}  // namespace

bool mp_parse_discovery_response(const char* bytes, size_t size, MPDiscoveryResponse& response) {
  const std::string prefix = std::string(DISCOVERY_MAGIC) + "|";
  if (!bytes || size <= prefix.size() || memcmp(bytes, prefix.data(), prefix.size()) != 0) {
    return false;
  }
  const std::string_view payload(bytes + prefix.size(), size - prefix.size());
  const size_t port_separator = payload.find('|');
  if (port_separator == std::string_view::npos ||
      payload.find('|', port_separator + 1) != std::string_view::npos) {
    return false;
  }
  uint32_t parsed_port = 0;
  const std::string_view port_text = payload.substr(0, port_separator);
  const auto port_result =
      std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed_port);
  if (port_result.ec != std::errc() || port_result.ptr != port_text.data() + port_text.size() ||
      !mp_valid_gameplay_port(parsed_port)) {
    return false;
  }
  std::string normalized_room_code;
  if (!mp_normalize_room_code(payload.substr(port_separator + 1), normalized_room_code, false)) {
    return false;
  }
  response.port = static_cast<uint16_t>(parsed_port);
  response.room_code = std::move(normalized_room_code);
  return true;
}

void MultiplayerScanner::start_search(MultiplayerData& data) {
  if (data.join_status == (int)MultiplayerStatus::SEARCHING) return;
  
  data.stop_search = false;
  data.connection_phase = static_cast<int>(MultiplayerConnectionPhase::CONTACTING_HOST);
  data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::NONE);
  {
    std::lock_guard<std::mutex> lock(data.discovery_result_mutex);
    data.found_ip.clear();
    data.directed_discovery = false;
    data.directed_discovery_address = 0;
    data.directed_discovery_game_port = 0;
  }
  if (data.scanner_thread.joinable()) {
    data.scanner_thread.join();
  }
  data.scanner_thread = std::thread(scan_thread_func, &data);
}

bool MultiplayerScanner::start_direct_search(MultiplayerData& data,
                                             const std::string& address,
                                             uint16_t game_port) {
  sockaddr_in target = {};
  target.sin_family = AF_INET;
  if (game_port == 0 || inet_pton(AF_INET, address.c_str(), &target.sin_addr) != 1) {
    return false;
  }
  data.stop_search = true;
  if (data.scanner_thread.joinable()) {
    data.scanner_thread.join();
  }
  data.stop_search = false;
  data.connection_phase = static_cast<int>(MultiplayerConnectionPhase::CONTACTING_HOST);
  data.connection_failure = static_cast<int>(MultiplayerConnectionFailure::NONE);
  {
    std::lock_guard<std::mutex> lock(data.discovery_result_mutex);
    data.found_ip.clear();
    data.directed_discovery = true;
    data.directed_discovery_address = target.sin_addr.s_addr;
    data.directed_discovery_game_port = game_port;
  }
  data.scanner_thread = std::thread(scan_thread_func, &data);
  return true;
}

void MultiplayerScanner::stop_search(MultiplayerData& data) {
  data.stop_search = true;
  data.join_status = (int)MultiplayerStatus::IDLE;
  if (data.scanner_thread.joinable()) {
    data.scanner_thread.join();
  }
  std::lock_guard<std::mutex> lock(data.discovery_result_mutex);
  mp_secure_clear_string(data.found_ip);
  data.directed_discovery = false;
  data.directed_discovery_address = 0;
  data.directed_discovery_game_port = 0;
}

int MultiplayerScanner::get_status(const MultiplayerData& data) {
  return data.join_status;
}

void MultiplayerScanner::scan_thread_func(MultiplayerData* data) {
  data->join_status = (int)MultiplayerStatus::SEARCHING;
  bool directed = false;
  uint32_t directed_address = 0;
  uint16_t game_port = kDefaultMultiplayerPort;
  {
    std::lock_guard<std::mutex> lock(data->discovery_result_mutex);
    directed = data->directed_discovery;
    directed_address = data->directed_discovery_address;
    if (directed && data->directed_discovery_game_port != 0) {
      game_port = data->directed_discovery_game_port;
    }
  }
  
  int sock = open_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    data->join_status = (int)MultiplayerStatus::FAILED;
    data->connection_failure =
        static_cast<int>(MultiplayerConnectionFailure::HOST_UNREACHABLE);
    return;
  }

  // Enable broadcasting
  int broadcast_enable = 1;
  set_socket_option(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));
  set_socket_timeout(sock, 500000); // 500ms timeout

  std::vector<sockaddr_in> discovery_targets;
  if (directed) {
    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(DISCOVERY_PORT);
    target.sin_addr.s_addr = directed_address;
    discovery_targets.assign(1, target);
    lg::info("[Multiplayer] Starting directed host discovery.");
  } else {
    discovery_targets = get_lan_discovery_targets();
    lg::info("[Multiplayer] Starting LAN discovery on port {} across {} broadcast target(s)...",
             DISCOVERY_PORT, discovery_targets.size());
  }

  const int max_attempts = 10;
  for (int attempt = 0; attempt < max_attempts && !data->stop_search; ++attempt) {
    for (const auto& target : discovery_targets) {
      sendto(sock, DISCOVERY_MAGIC, strlen(DISCOVERY_MAGIC), 0, (const sockaddr*)&target,
             sizeof(target));
    }

    // Wait for reply
    char buffer[128];
    sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    int bytes_received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&from_addr, &from_len);
    if (bytes_received > 0) {
      MPDiscoveryResponse response;
      const bool expected_source = ntohs(from_addr.sin_port) == DISCOVERY_PORT &&
                                   (!directed || from_addr.sin_addr.s_addr == directed_address);
      if (expected_source &&
          mp_parse_discovery_response(buffer, static_cast<size_t>(bytes_received), response) &&
          (!directed || response.port == game_port)) {
        const std::string found_ip = address_to_string(from_addr);
        std::string found_invite = "jad2mp://" + found_ip + ":" +
                                   std::to_string(response.port) + "/" + response.room_code;
        {
          std::lock_guard<std::mutex> lock(data->discovery_result_mutex);
          data->found_ip = found_invite;
        }
        mp_secure_clear_string(found_invite);
        mp_secure_clear_string(response.room_code);
        lg::info("[Multiplayer] Found a compatible discovery responder.");
        data->join_status = (int)MultiplayerStatus::FOUND;
        close_socket(sock);
        return;
      }
    }
  }

  lg::info("[Multiplayer] Discovery timed out.");
  if (data->join_status == (int)MultiplayerStatus::SEARCHING) {
    data->join_status = directed ? (int)MultiplayerStatus::CREDENTIAL_DISCOVERY_FAILED
                                 : (int)MultiplayerStatus::FAILED;
    data->connection_failure = static_cast<int>(
        directed ? MultiplayerConnectionFailure::CREDENTIAL_DISCOVERY_FAILED
                 : MultiplayerConnectionFailure::LAN_TIMEOUT);
  }
  close_socket(sock);
}
