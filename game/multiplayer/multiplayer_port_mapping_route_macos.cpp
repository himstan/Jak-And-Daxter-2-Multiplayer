#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>
#include <utility>

#include "game/multiplayer/multiplayer_port_mapping_route.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace {
constexpr const char* kRouteProbeAddress = "1.1.1.1";

class SocketHandle {
 public:
  explicit SocketHandle(int socket) : m_socket(socket) {}
  ~SocketHandle() {
    if (m_socket >= 0) {
      close(m_socket);
    }
  }
  int get() const { return m_socket; }

 private:
  int m_socket = -1;
};

std::string system_error(const char* operation, int error_number = errno) {
  return std::string(operation) + " failed: errno " + std::to_string(error_number) + " (" +
         std::strerror(error_number) + ")";
}

size_t aligned_sockaddr_size(const sockaddr* address) {
  constexpr size_t alignment = sizeof(uintptr_t);
  const size_t length = address->sa_len;
  return length == 0 ? alignment : ((length + alignment - 1) & ~(alignment - 1));
}

bool find_local_address_for_destination(std::string& local_ip, std::string& error) {
  SocketHandle socket_handle(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (socket_handle.get() < 0) {
    error = system_error("creating the IPv4 route-probe socket");
    return false;
  }

  sockaddr_in destination = {};
  destination.sin_len = sizeof(destination);
  destination.sin_family = AF_INET;
  destination.sin_port = htons(9);
  if (inet_pton(AF_INET, kRouteProbeAddress, &destination.sin_addr) != 1) {
    error = "preparing the IPv4 route-probe destination failed";
    return false;
  }
  if (connect(socket_handle.get(), reinterpret_cast<const sockaddr*>(&destination),
              sizeof(destination)) != 0) {
    error = system_error("selecting the local IPv4 route-probe address");
    return false;
  }

  sockaddr_in local = {};
  socklen_t local_size = sizeof(local);
  if (getsockname(socket_handle.get(), reinterpret_cast<sockaddr*>(&local), &local_size) != 0) {
    error = system_error("reading the local IPv4 route-probe address");
    return false;
  }
  char address[INET_ADDRSTRLEN] = {};
  if (!inet_ntop(AF_INET, &local.sin_addr, address, sizeof(address))) {
    error = system_error("converting the local IPv4 route-probe address");
    return false;
  }
  local_ip = address;
  return true;
}
}  // namespace

bool mp_find_preferred_ipv4_route(NetworkAdapterInfo& out, std::string& error) {
  SocketHandle route_socket(socket(PF_ROUTE, SOCK_RAW, AF_INET));
  if (route_socket.get() < 0) {
    error = system_error("opening the macOS routing socket");
    return false;
  }

  struct {
    rt_msghdr header;
    sockaddr_in destination;
  } request = {};
  request.header.rtm_msglen = sizeof(request);
  request.header.rtm_version = RTM_VERSION;
  request.header.rtm_type = RTM_GET;
  request.header.rtm_addrs = RTA_DST;
  request.header.rtm_pid = getpid();
  request.header.rtm_seq = 1;
  request.destination.sin_len = sizeof(request.destination);
  request.destination.sin_family = AF_INET;
  if (inet_pton(AF_INET, kRouteProbeAddress, &request.destination.sin_addr) != 1) {
    error = "preparing the macOS IPv4 routing-table query failed";
    return false;
  }

  if (write(route_socket.get(), &request, sizeof(request)) !=
      static_cast<ssize_t>(sizeof(request))) {
    error = system_error("sending the macOS IPv4 routing-table query");
    return false;
  }

  char response[2048] = {};
  rt_msghdr* header = nullptr;
  while (true) {
    const ssize_t received = read(route_socket.get(), response, sizeof(response));
    if (received < 0) {
      error = system_error("receiving the macOS IPv4 routing-table response");
      return false;
    }
    if (static_cast<size_t>(received) < sizeof(rt_msghdr)) {
      error = "macOS returned a truncated IPv4 routing-table response";
      return false;
    }
    header = reinterpret_cast<rt_msghdr*>(response);
    if (header->rtm_msglen < sizeof(rt_msghdr) ||
        header->rtm_msglen > static_cast<size_t>(received)) {
      error = "macOS returned an invalid IPv4 routing-table message length";
      return false;
    }
    if (header->rtm_version == RTM_VERSION && header->rtm_pid == request.header.rtm_pid &&
        header->rtm_seq == request.header.rtm_seq) {
      break;
    }
  }
  if (header->rtm_errno != 0) {
    error = system_error("macOS IPv4 route lookup", header->rtm_errno);
    return false;
  }
  if (header->rtm_index == 0) {
    error = "macOS returned no usable preferred IPv4 interface";
    return false;
  }

  const char* cursor = response + sizeof(rt_msghdr);
  const char* response_end = response + header->rtm_msglen;
  const sockaddr* addresses[RTAX_MAX] = {};
  for (int index = 0; index < RTAX_MAX; ++index) {
    if ((header->rtm_addrs & (1 << index)) == 0) {
      continue;
    }
    if (cursor + sizeof(sockaddr) > response_end) {
      error = "macOS returned malformed IPv4 route address data";
      return false;
    }
    const auto* address = reinterpret_cast<const sockaddr*>(cursor);
    const size_t address_size = aligned_sockaddr_size(address);
    if (address_size == 0 || cursor + address_size > response_end) {
      error = "macOS returned malformed IPv4 route address length";
      return false;
    }
    addresses[index] = address;
    cursor += address_size;
  }

  const sockaddr* gateway_address = addresses[RTAX_GATEWAY];
  if (!gateway_address || gateway_address->sa_family != AF_INET ||
      (header->rtm_flags & RTF_GATEWAY) == 0) {
    error = "preferred IPv4 route uses interface " + std::to_string(header->rtm_index) +
            " but has no usable next-hop gateway (the route may be VPN or on-link)";
    return false;
  }
  const auto* gateway = reinterpret_cast<const sockaddr_in*>(gateway_address);
  if (gateway->sin_addr.s_addr == INADDR_ANY) {
    error = "preferred IPv4 route returned an empty next-hop gateway";
    return false;
  }

  char interface_name[IF_NAMESIZE] = {};
  if (!if_indextoname(header->rtm_index, interface_name)) {
    error = system_error("resolving the preferred macOS interface name");
    return false;
  }
  char gateway_ip[INET_ADDRSTRLEN] = {};
  if (!inet_ntop(AF_INET, &gateway->sin_addr, gateway_ip, sizeof(gateway_ip))) {
    error = system_error("converting the preferred macOS gateway address");
    return false;
  }
  std::string local_ip;
  if (!find_local_address_for_destination(local_ip, error)) {
    return false;
  }

  out.name = interface_name;
  out.local_ip = std::move(local_ip);
  out.gateway_ip = gateway_ip;
  out.interface_index = header->rtm_index;
  out.route_metric.reset();
  out.interface_metric.reset();
  return true;
}
