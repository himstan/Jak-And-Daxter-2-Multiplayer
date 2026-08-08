#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>

#include "game/multiplayer/multiplayer_port_mapping_route.h"
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

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

bool find_local_address_for_destination(std::string& local_ip, std::string& error) {
  SocketHandle socket_handle(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (socket_handle.get() < 0) {
    error = system_error("creating the IPv4 route-probe socket");
    return false;
  }

  sockaddr_in destination = {};
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

bool parse_multipath(const rtattr* attribute, uint32_t& interface_index, in_addr& gateway) {
  int remaining = RTA_PAYLOAD(attribute);
  auto* next_hop = reinterpret_cast<const rtnexthop*>(RTA_DATA(attribute));
  while (RTNH_OK(next_hop, remaining)) {
    int attributes_size = next_hop->rtnh_len - RTNH_LENGTH(0);
    for (auto* nested = RTNH_DATA(next_hop); RTA_OK(nested, attributes_size);
         nested = RTA_NEXT(nested, attributes_size)) {
      if (nested->rta_type == RTA_GATEWAY && RTA_PAYLOAD(nested) >= sizeof(gateway)) {
        std::memcpy(&gateway, RTA_DATA(nested), sizeof(gateway));
        interface_index = static_cast<uint32_t>(next_hop->rtnh_ifindex);
        return true;
      }
    }
    remaining -= RTNH_ALIGN(next_hop->rtnh_len);
    next_hop = RTNH_NEXT(next_hop);
  }
  return false;
}
}  // namespace

bool mp_find_preferred_ipv4_route(NetworkAdapterInfo& out, std::string& error) {
  SocketHandle route_socket(socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE));
  if (route_socket.get() < 0) {
    error = system_error("opening the Linux routing socket");
    return false;
  }

  timeval receive_timeout = {};
  receive_timeout.tv_sec = 1;
  if (setsockopt(route_socket.get(), SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                 sizeof(receive_timeout)) != 0) {
    error = system_error("configuring the Linux routing response timeout");
    return false;
  }

  struct {
    nlmsghdr header;
    rtmsg route;
    char attributes[RTA_SPACE(sizeof(in_addr))];
  } request = {};
  request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
  request.header.nlmsg_type = RTM_GETROUTE;
  request.header.nlmsg_flags = NLM_F_REQUEST;
  request.header.nlmsg_seq = 1;
  request.route.rtm_family = AF_INET;
  request.route.rtm_dst_len = 32;

  auto* destination_attribute = reinterpret_cast<rtattr*>(reinterpret_cast<char*>(&request) +
                                                          NLMSG_ALIGN(request.header.nlmsg_len));
  destination_attribute->rta_type = RTA_DST;
  destination_attribute->rta_len = RTA_LENGTH(sizeof(in_addr));
  if (inet_pton(AF_INET, kRouteProbeAddress, RTA_DATA(destination_attribute)) != 1) {
    error = "preparing the Linux IPv4 routing-table query failed";
    return false;
  }
  request.header.nlmsg_len = NLMSG_ALIGN(request.header.nlmsg_len) + RTA_LENGTH(sizeof(in_addr));

  sockaddr_nl kernel = {};
  kernel.nl_family = AF_NETLINK;
  if (sendto(route_socket.get(), &request, request.header.nlmsg_len, 0,
             reinterpret_cast<const sockaddr*>(&kernel), sizeof(kernel)) < 0) {
    error = system_error("sending the Linux IPv4 routing-table query");
    return false;
  }

  uint32_t interface_index = 0;
  in_addr gateway = {};
  in_addr preferred_source = {};
  std::optional<uint32_t> route_metric;
  bool found_route = false;
  char response[8192] = {};
  while (!found_route) {
    const ssize_t received = recv(route_socket.get(), response, sizeof(response), 0);
    if (received < 0) {
      error = system_error("receiving the Linux IPv4 routing-table response");
      return false;
    }

    unsigned int remaining = static_cast<unsigned int>(received);
    for (auto* header = reinterpret_cast<nlmsghdr*>(response); NLMSG_OK(header, remaining);
         header = NLMSG_NEXT(header, remaining)) {
      if (header->nlmsg_seq != request.header.nlmsg_seq) {
        continue;
      }
      if (header->nlmsg_type == NLMSG_ERROR) {
        const auto* netlink_error = reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(header));
        const int error_number = netlink_error->error < 0 ? -netlink_error->error : EIO;
        error = system_error("Linux IPv4 route lookup", error_number);
        return false;
      }
      if (header->nlmsg_type == NLMSG_DONE) {
        break;
      }
      if (header->nlmsg_type != RTM_NEWROUTE) {
        continue;
      }

      const auto* route = reinterpret_cast<const rtmsg*>(NLMSG_DATA(header));
      if (route->rtm_family != AF_INET || route->rtm_type != RTN_UNICAST) {
        continue;
      }
      int attributes_size = RTM_PAYLOAD(header);
      for (auto* attribute = RTM_RTA(route); RTA_OK(attribute, attributes_size);
           attribute = RTA_NEXT(attribute, attributes_size)) {
        switch (attribute->rta_type) {
          case RTA_OIF:
            if (RTA_PAYLOAD(attribute) >= sizeof(interface_index)) {
              std::memcpy(&interface_index, RTA_DATA(attribute), sizeof(interface_index));
            }
            break;
          case RTA_GATEWAY:
            if (RTA_PAYLOAD(attribute) >= sizeof(gateway)) {
              std::memcpy(&gateway, RTA_DATA(attribute), sizeof(gateway));
            }
            break;
          case RTA_PREFSRC:
            if (RTA_PAYLOAD(attribute) >= sizeof(preferred_source)) {
              std::memcpy(&preferred_source, RTA_DATA(attribute), sizeof(preferred_source));
            }
            break;
          case RTA_PRIORITY: {
            uint32_t metric = 0;
            if (RTA_PAYLOAD(attribute) >= sizeof(metric)) {
              std::memcpy(&metric, RTA_DATA(attribute), sizeof(metric));
              route_metric = metric;
            }
            break;
          }
          case RTA_MULTIPATH:
            if (gateway.s_addr == INADDR_ANY) {
              (void)parse_multipath(attribute, interface_index, gateway);
            }
            break;
          default:
            break;
        }
      }
      found_route = true;
      break;
    }
  }

  if (!found_route || interface_index == 0) {
    error = "Linux returned no usable preferred IPv4 route";
    return false;
  }
  if (gateway.s_addr == INADDR_ANY) {
    error = "preferred IPv4 route uses interface " + std::to_string(interface_index) +
            " but has no usable next-hop gateway (the route may be VPN or on-link)";
    return false;
  }

  char interface_name[IF_NAMESIZE] = {};
  if (!if_indextoname(interface_index, interface_name)) {
    error = system_error("resolving the preferred Linux interface name");
    return false;
  }
  char gateway_ip[INET_ADDRSTRLEN] = {};
  if (!inet_ntop(AF_INET, &gateway, gateway_ip, sizeof(gateway_ip))) {
    error = system_error("converting the preferred Linux gateway address");
    return false;
  }

  std::string local_ip;
  if (preferred_source.s_addr != INADDR_ANY) {
    char source_ip[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &preferred_source, source_ip, sizeof(source_ip))) {
      error = system_error("converting the preferred Linux local address");
      return false;
    }
    local_ip = source_ip;
  } else if (!find_local_address_for_destination(local_ip, error)) {
    return false;
  }

  out.name = interface_name;
  out.local_ip = std::move(local_ip);
  out.gateway_ip = gateway_ip;
  out.interface_index = interface_index;
  out.route_metric = route_metric;
  out.interface_metric.reset();
  return true;
}
