#include "multiplayer_scanner.h"
#include "multiplayer_protocol.h"
#include "common/cross_sockets/XSocket.h"
#include "common/log/log.h"
#include <chrono>
#include <string>
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

void MultiplayerScanner::start_search(MultiplayerData& data) {
  if (data.join_status == (int)MultiplayerStatus::SEARCHING) return;
  
  data.stop_search = false;
  if (data.scanner_thread.joinable()) {
    data.scanner_thread.join();
  }
  data.scanner_thread = std::thread(scan_thread_func, &data);
}

void MultiplayerScanner::stop_search(MultiplayerData& data) {
  data.stop_search = true;
  data.join_status = (int)MultiplayerStatus::IDLE;
  if (data.scanner_thread.joinable()) {
    data.scanner_thread.join();
  }
}

int MultiplayerScanner::get_status(const MultiplayerData& data) {
  return data.join_status;
}

void MultiplayerScanner::scan_thread_func(MultiplayerData* data) {
  data->join_status = (int)MultiplayerStatus::SEARCHING;
  
  int sock = open_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    data->join_status = (int)MultiplayerStatus::FAILED;
    return;
  }

  // Enable broadcasting
  int broadcast_enable = 1;
  set_socket_option(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));
  set_socket_timeout(sock, 500000); // 500ms timeout

  const auto discovery_targets = get_lan_discovery_targets();
  lg::info("[Multiplayer] Starting LAN discovery on port {} across {} broadcast target(s)...",
           DISCOVERY_PORT, discovery_targets.size());

  const int max_attempts = 10;
  for (int attempt = 0; attempt < max_attempts && !data->stop_search; ++attempt) {
    for (const auto& target : discovery_targets) {
      sendto(sock, DISCOVERY_MAGIC, strlen(DISCOVERY_MAGIC), 0, (const sockaddr*)&target,
             sizeof(target));
    }

    // Wait for reply
    char buffer[64];
    sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    int bytes_received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&from_addr, &from_len);
    if (bytes_received > 0) {
      buffer[bytes_received] = '\0';
      if (std::string(buffer) == DISCOVERY_MAGIC) {
        data->found_ip = address_to_string(from_addr);
        lg::info("[Multiplayer] Found host at {}", data->found_ip);
        data->join_status = (int)MultiplayerStatus::FOUND;
        close_socket(sock);
        return;
      }
    }
  }

  lg::info("[Multiplayer] Discovery timed out.");
  if (data->join_status == (int)MultiplayerStatus::SEARCHING) {
    data->join_status = (int)MultiplayerStatus::FAILED;
  }
  close_socket(sock);
}
