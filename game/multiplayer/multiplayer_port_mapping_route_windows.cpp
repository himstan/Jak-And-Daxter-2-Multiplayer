#include "game/multiplayer/multiplayer_port_mapping_route.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// clang-format off: Windows networking headers have a required include order.
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
// clang-format on

#include <string>
#include <vector>

namespace {
std::string windows_error_message(DWORD error_code) {
  char* buffer = nullptr;
  const DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string message;
  if (length && buffer) {
    message.assign(buffer, length);
    while (!message.empty() &&
           (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
      message.pop_back();
    }
  }
  if (buffer) {
    LocalFree(buffer);
  }
  return message;
}

std::string format_win32_error(DWORD error_code) {
  std::string result = "Win32 error " + std::to_string(error_code);
  const auto message = windows_error_message(error_code);
  if (!message.empty()) {
    result += " (" + message + ")";
  }
  return result;
}

std::string wide_to_utf8(const wchar_t* value) {
  if (!value || !*value) {
    return {};
  }
  const int required_size =
      WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (required_size <= 1) {
    return {};
  }
  std::string result(required_size, '\0');
  if (!WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), required_size, nullptr, nullptr)) {
    return {};
  }
  result.pop_back();
  return result;
}
}  // namespace

bool mp_find_preferred_ipv4_route(NetworkAdapterInfo& out, std::string& error) {
  SOCKADDR_INET destination = {};
  destination.Ipv4.sin_family = AF_INET;
  if (InetPtonA(AF_INET, "1.1.1.1", &destination.Ipv4.sin_addr) != 1) {
    error = "preparing the IPv4 routing-table query failed";
    return false;
  }

  MIB_IPFORWARD_ROW2 best_route = {};
  SOCKADDR_INET best_source = {};
  const DWORD route_result =
      GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &best_route, &best_source);
  if (route_result != NO_ERROR) {
    error = "preferred IPv4 route lookup failed: " + format_win32_error(route_result);
    return false;
  }
  if (best_source.si_family != AF_INET || best_source.Ipv4.sin_addr.s_addr == htonl(INADDR_ANY)) {
    error = "preferred IPv4 route returned no usable local IPv4 address";
    return false;
  }
  if (best_route.NextHop.si_family != AF_INET ||
      best_route.NextHop.Ipv4.sin_addr.s_addr == htonl(INADDR_ANY)) {
    error = "preferred IPv4 route uses interface " + std::to_string(best_route.InterfaceIndex) +
            " but has no usable next-hop gateway (the route may be VPN or on-link)";
    return false;
  }

  char local_ip[INET_ADDRSTRLEN] = {};
  char gateway_ip[INET_ADDRSTRLEN] = {};
  if (!InetNtopA(AF_INET, &best_source.Ipv4.sin_addr, local_ip, sizeof(local_ip))) {
    error = "converting the preferred local IPv4 address failed: " +
            format_win32_error(WSAGetLastError());
    return false;
  }
  if (!InetNtopA(AF_INET, &best_route.NextHop.Ipv4.sin_addr, gateway_ip, sizeof(gateway_ip))) {
    error = "converting the preferred gateway IPv4 address failed: " +
            format_win32_error(WSAGetLastError());
    return false;
  }

  constexpr ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                          GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  std::vector<uint8_t> adapter_buffer(15 * 1024);
  IP_ADAPTER_ADDRESSES* adapters = nullptr;
  ULONG adapter_result = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 3 && adapter_result == ERROR_BUFFER_OVERFLOW; ++attempt) {
    ULONG buffer_size = static_cast<ULONG>(adapter_buffer.size());
    adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapter_buffer.data());
    adapter_result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buffer_size);
    if (adapter_result == ERROR_BUFFER_OVERFLOW) {
      adapter_buffer.resize(buffer_size);
      adapters = nullptr;
    }
  }
  if (adapter_result != NO_ERROR || !adapters) {
    error = "adapter metadata discovery failed: " + format_win32_error(adapter_result);
    return false;
  }

  for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
    if (adapter->IfIndex != best_route.InterfaceIndex) {
      continue;
    }
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
      error = "preferred IPv4 route selected unavailable interface " +
              std::to_string(best_route.InterfaceIndex);
      return false;
    }

    out.name = wide_to_utf8(adapter->FriendlyName);
    if (out.name.empty() && adapter->AdapterName) {
      out.name = adapter->AdapterName;
    }
    if (out.name.empty()) {
      out.name = "<unnamed>";
    }
    out.local_ip = local_ip;
    out.gateway_ip = gateway_ip;
    out.interface_index = best_route.InterfaceIndex;
    out.route_metric = best_route.Metric;
    out.interface_metric = adapter->Ipv4Metric;
    return true;
  }

  error = "preferred IPv4 route uses interface " + std::to_string(best_route.InterfaceIndex) +
          ", but its adapter metadata was not found";
  return false;
}
