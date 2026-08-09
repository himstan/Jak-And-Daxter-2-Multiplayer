#include "game/multiplayer/multiplayer_port_mapping.h"

#include "common/log/log.h"

#include "game/multiplayer/multiplayer_port_mapping_internal.h"
#include "game/multiplayer/multiplayer_port_mapping_route.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// clang-format off: Windows networking headers have a required include order.
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
// clang-format on
#else
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "miniupnpc.h"
#include "natpmp.h"
#include "upnpcommands.h"
#include "upnperrors.h"

struct MPPortMappingContext {
  MPPortMappingMethod method = MPPortMappingMethod::NONE;
  NetworkAdapterInfo route;
  std::string upnp_control_url;
  std::string upnp_service_type;
};

namespace {
constexpr uint32_t kPortMappingLeaseSeconds = 7200;
constexpr int kUpnpDiscoveryDelayMilliseconds = 2000;
constexpr auto kNatPmpOperationDeadline = std::chrono::milliseconds(2500);

struct PortMappingAttemptResult {
  bool success = false;
  std::string external_ip;
  std::string error;
  std::shared_ptr<MPPortMappingContext> context;
};

bool parse_ipv4_host_order(const std::string& address, uint32_t& output) {
  uint32_t octets[4] = {};
  size_t octet = 0;
  bool has_digit = false;
  for (const char character : address) {
    if (character >= '0' && character <= '9') {
      has_digit = true;
      octets[octet] = octets[octet] * 10 + static_cast<uint32_t>(character - '0');
      if (octets[octet] > 255) {
        return false;
      }
    } else if (character == '.' && has_digit && octet < 3) {
      ++octet;
      has_digit = false;
    } else {
      return false;
    }
  }
  if (!has_digit || octet != 3) {
    return false;
  }
  output = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
  return true;
}

#ifdef _WIN32
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

int last_socket_error() {
  return WSAGetLastError();
}

std::string format_socket_error(int error_code) {
  std::string result = "Winsock error " + std::to_string(error_code);
  const auto message = windows_error_message(static_cast<DWORD>(error_code));
  if (!message.empty()) {
    result += " (" + message + ")";
  }
  return result;
}

bool initialize_socket_runtime(std::string& error) {
  static std::once_flag once;
  static int startup_result = WSASYSNOTREADY;
  std::call_once(once, []() {
    WSADATA data = {};
    startup_result = WSAStartup(MAKEWORD(2, 2), &data);
  });
  if (startup_result != 0) {
    error = "Winsock initialization failed: " + format_socket_error(startup_result);
    return false;
  }
  return true;
}
#else
int last_socket_error() {
  return errno;
}

std::string format_socket_error(int error_code) {
  return "errno " + std::to_string(error_code) + " (" + std::strerror(error_code) + ")";
}

bool initialize_socket_runtime(std::string&) {
  return true;
}
#endif

std::string describe_upnp_discovery_error(int error) {
  switch (error) {
    case UPNPDISCOVER_SUCCESS:
      return "no UPnP devices responded to SSDP discovery";
    case UPNPDISCOVER_SOCKET_ERROR:
      return "SSDP socket setup or communication failed";
    case UPNPDISCOVER_MEMORY_ERROR:
      return "SSDP discovery ran out of memory";
    case UPNPDISCOVER_UNKNOWN_ERROR:
      return "SSDP discovery failed with an unknown error";
    default:
      return "SSDP discovery failed with MiniUPnPc error " + std::to_string(error);
  }
}

std::string describe_igd_status(int status) {
  switch (status) {
    case UPNP_NO_IGD:
      return "no UPnP Internet Gateway Device was found";
    case UPNP_CONNECTED_IGD:
      return "connected IGD";
    case UPNP_PRIVATEIP_IGD:
      return "IGD reports a private WAN address";
    case UPNP_DISCONNECTED_IGD:
      return "IGD reports that its WAN connection is disconnected";
    case UPNP_UNKNOWN_DEVICE:
      return "discovered UPnP device was not recognized as an IGD";
    default:
      return "unexpected IGD status " + std::to_string(status);
  }
}

void log_port_mapping_route(const NetworkAdapterInfo& route, bool initial_attempt) {
  const auto description = mp_format_port_mapping_route(route);
  if (initial_attempt) {
    lg::info("[Multiplayer] Automatic UDP port-mapping route: {}.", description);
  } else {
    lg::debug("[Multiplayer] Automatic UDP port-mapping route: {}.", description);
  }
}

struct UpnpDeviceListDeleter {
  void operator()(UPNPDev* devices) const {
    if (devices) {
      freeUPNPDevlist(devices);
    }
  }
};

class UpnpUrlsHandle {
 public:
  UPNPUrls* get() { return &m_urls; }
  ~UpnpUrlsHandle() { FreeUPNPUrls(&m_urls); }

 private:
  UPNPUrls m_urls = {};
};

PortMappingAttemptResult upnp_add_mapping(const NetworkAdapterInfo& route,
                                          uint16_t local_port,
                                          uint16_t external_port) {
  int discovery_error = UPNPDISCOVER_UNKNOWN_ERROR;
  std::unique_ptr<UPNPDev, UpnpDeviceListDeleter> devices(
      upnpDiscover(kUpnpDiscoveryDelayMilliseconds, route.local_ip.c_str(), nullptr,
                   UPNP_LOCAL_PORT_ANY, 0, 2, &discovery_error));
  if (!devices) {
    std::string error = describe_upnp_discovery_error(discovery_error);
    if (discovery_error == UPNPDISCOVER_SOCKET_ERROR) {
      error += ": " + format_socket_error(last_socket_error());
    }
    return {false, {}, std::move(error), {}};
  }

  UpnpUrlsHandle urls;
  IGDdatas data = {};
  char discovered_local_address[64] = {};
  char discovered_wan_address[64] = {};
  const int igd_status = UPNP_GetValidIGD(
      devices.get(), urls.get(), &data, discovered_local_address, sizeof(discovered_local_address),
      discovered_wan_address, sizeof(discovered_wan_address));
  if (igd_status != UPNP_CONNECTED_IGD && igd_status != UPNP_PRIVATEIP_IGD) {
    return {false, discovered_wan_address, describe_igd_status(igd_status), {}};
  }
  if (!urls.get()->controlURL || !*urls.get()->controlURL || !*data.first.servicetype) {
    return {false,
            discovered_wan_address,
            "the selected UPnP IGD returned no usable control endpoint",
            {}};
  }

  const std::string local_port_text = std::to_string(local_port);
  const std::string external_port_text = std::to_string(external_port);
  const int add_result =
      UPNP_AddPortMapping(urls.get()->controlURL, data.first.servicetype,
                          external_port_text.c_str(), local_port_text.c_str(),
                          route.local_ip.c_str(), "OpenGOAL Jak II Multiplayer", "UDP", "", "0");
  if (add_result != UPNPCOMMAND_SUCCESS) {
    return {false,
            discovered_wan_address,
            "adding UDP " + external_port_text + " -> " + route.local_ip + ":" + local_port_text +
                " failed: " + mp_describe_upnp_result(add_result),
            {}};
  }

  auto context = std::make_shared<MPPortMappingContext>();
  context->method = MPPortMappingMethod::UPNP_IGD;
  context->route = route;
  context->upnp_control_url = urls.get()->controlURL;
  context->upnp_service_type = data.first.servicetype;

  char external_address[64] = {};
  const int address_result = UPNP_GetExternalIPAddress(
      context->upnp_control_url.c_str(), context->upnp_service_type.c_str(), external_address);
  if (address_result != UPNPCOMMAND_SUCCESS) {
    return {true,
            {},
            "mapping was added, but reading its external IPv4 address failed: " +
                mp_describe_upnp_result(address_result),
            std::move(context)};
  }
  if (!*external_address) {
    return {true,
            {},
            "mapping was added, but the router returned no external IPv4 address",
            std::move(context)};
  }
  return {true, external_address, {}, std::move(context)};
}

void upnp_delete_mapping(const MPPortMappingContext& context, uint16_t external_port) {
  if (context.upnp_control_url.empty() || context.upnp_service_type.empty()) {
    lg::debug("[Multiplayer] UPnP cleanup skipped because the original IGD endpoint is missing.");
    return;
  }
  const std::string external_port_text = std::to_string(external_port);
  const int result =
      UPNP_DeletePortMapping(context.upnp_control_url.c_str(), context.upnp_service_type.c_str(),
                             external_port_text.c_str(), "UDP", "");
  if (result != UPNPCOMMAND_SUCCESS) {
    lg::debug("[Multiplayer] UPnP cleanup for UDP port {} failed: {}.", external_port,
              mp_describe_upnp_result(result));
  }
}

class NatPmpClient {
 public:
  ~NatPmpClient() {
    if (m_open) {
      (void)closenatpmp(&m_client);
    }
  }

  bool open(const std::string& gateway_ip, std::string& error) {
    in_addr gateway = {};
    if (inet_pton(AF_INET, gateway_ip.c_str(), &gateway) != 1) {
      error = "invalid selected gateway IPv4 address " + gateway_ip;
      return false;
    }

    const int result = initnatpmp(&m_client, 1, gateway.s_addr);
    if (result != 0) {
      const int socket_error = last_socket_error();
      if (result != NATPMP_ERR_SOCKETERROR && result != NATPMP_ERR_INVALIDARGS) {
        (void)closenatpmp(&m_client);
      }
      error = mp_describe_natpmp_result(result);
      if (result == NATPMP_ERR_SOCKETERROR || result == NATPMP_ERR_CONNECTERR ||
          result == NATPMP_ERR_FCNTLERROR) {
        error += ": " + format_socket_error(socket_error);
      }
      return false;
    }
    m_open = true;
    return true;
  }

  natpmp_t* get() { return &m_client; }

 private:
  natpmp_t m_client = {};
  bool m_open = false;
};

bool wait_for_natpmp_response(NatPmpClient& client,
                              natpmpresp_t& response,
                              const std::string& gateway_ip,
                              std::string& error) {
  const auto deadline = std::chrono::steady_clock::now() + kNatPmpOperationDeadline;
  bool socket_became_readable = false;

  while (std::chrono::steady_clock::now() < deadline) {
    timeval timeout = {};
    const int timeout_result = getnatpmprequesttimeout(client.get(), &timeout);
    if (timeout_result < 0) {
      error = mp_describe_natpmp_result(timeout_result);
      return false;
    }
    if (timeout.tv_sec < 0 || (timeout.tv_sec == 0 && timeout.tv_usec < 0)) {
      timeout.tv_sec = 0;
      timeout.tv_usec = 0;
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - std::chrono::steady_clock::now());
    const auto requested =
        std::chrono::seconds(timeout.tv_sec) + std::chrono::microseconds(timeout.tv_usec);
    if (requested > remaining) {
      timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
      timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
    }

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(client.get()->s, &read_set);
#ifdef _WIN32
    const int select_result = select(0, &read_set, nullptr, nullptr, &timeout);
#else
    const int select_result = select(client.get()->s + 1, &read_set, nullptr, nullptr, &timeout);
#endif
    if (select_result < 0) {
      error = "waiting for a NAT-PMP response failed: " + format_socket_error(last_socket_error());
      return false;
    }
    socket_became_readable |= select_result > 0;

    const int result = readnatpmpresponseorretry(client.get(), &response);
    if (result == 0) {
      return true;
    }
    if (result == NATPMP_TRYAGAIN) {
      continue;
    }
    if (result == NATPMP_ERR_NOGATEWAYSUPPORT && !socket_became_readable) {
      error =
          "timed out waiting for a NAT-PMP response from " + gateway_ip + ":5351 after 3 attempts";
      return false;
    }

    const int socket_error = last_socket_error();
    error = mp_describe_natpmp_result(result);
    if (result == NATPMP_ERR_RECVFROM || result == NATPMP_ERR_SENDERR) {
      error += ": " + format_socket_error(socket_error);
    }
    return false;
  }

  error = "timed out waiting for a NAT-PMP response from " + gateway_ip + ":5351";
  return false;
}

PortMappingAttemptResult natpmp_request_mapping(const NetworkAdapterInfo& route,
                                                uint16_t local_port,
                                                uint16_t external_port,
                                                uint32_t lifetime_seconds) {
  NatPmpClient client;
  std::string client_error;
  if (!client.open(route.gateway_ip, client_error)) {
    return {false,
            {},
            "gateway " + route.gateway_ip + ":5351 initialization failed: " + client_error,
            {}};
  }

  const int send_result = sendnewportmappingrequest(client.get(), NATPMP_PROTOCOL_UDP, local_port,
                                                    external_port, lifetime_seconds);
  if (send_result < 0) {
    const int socket_error = last_socket_error();
    std::string error = mp_describe_natpmp_result(send_result);
    if (send_result == NATPMP_ERR_SENDERR) {
      error += ": " + format_socket_error(socket_error);
    }
    return {
        false, {}, "gateway " + route.gateway_ip + ":5351 mapping request failed: " + error, {}};
  }

  natpmpresp_t response = {};
  std::string response_error;
  if (!wait_for_natpmp_response(client, response, route.gateway_ip, response_error)) {
    return {false,
            {},
            "gateway " + route.gateway_ip + ":5351 mapping response failed: " + response_error,
            {}};
  }
  if (response.type != NATPMP_RESPTYPE_UDPPORTMAPPING) {
    return {false,
            {},
            "gateway " + route.gateway_ip + ":5351 returned unexpected response type " +
                std::to_string(response.type) + " for a UDP mapping",
            {}};
  }
  if (lifetime_seconds != 0 && response.pnu.newportmapping.mappedpublicport != external_port) {
    const uint16_t mapped_port = response.pnu.newportmapping.mappedpublicport;
    (void)natpmp_request_mapping(route, local_port, mapped_port, 0);
    return {false,
            {},
            "gateway " + route.gateway_ip + ":5351 mapped unexpected external UDP port " +
                std::to_string(mapped_port) + " instead of " + std::to_string(external_port),
            {}};
  }
  return {true, {}, {}, {}};
}

PortMappingAttemptResult natpmp_query_external_ip(const NetworkAdapterInfo& route) {
  NatPmpClient client;
  std::string client_error;
  if (!client.open(route.gateway_ip, client_error)) {
    return {false,
            {},
            "gateway " + route.gateway_ip + ":5351 initialization failed: " + client_error,
            {}};
  }

  const int send_result = sendpublicaddressrequest(client.get());
  if (send_result < 0) {
    const int socket_error = last_socket_error();
    std::string error = mp_describe_natpmp_result(send_result);
    if (send_result == NATPMP_ERR_SENDERR) {
      error += ": " + format_socket_error(socket_error);
    }
    return {false,
            {},
            "gateway " + route.gateway_ip + ":5351 external-address request failed: " + error,
            {}};
  }

  natpmpresp_t response = {};
  std::string response_error;
  if (!wait_for_natpmp_response(client, response, route.gateway_ip, response_error)) {
    return {
        false,
        {},
        "gateway " + route.gateway_ip + ":5351 external-address response failed: " + response_error,
        {}};
  }
  if (response.type != NATPMP_RESPTYPE_PUBLICADDRESS) {
    return {false,
            {},
            "gateway " + route.gateway_ip + ":5351 returned unexpected response type " +
                std::to_string(response.type) + " for an external-address request",
            {}};
  }

  char external_address[INET_ADDRSTRLEN] = {};
  if (!inet_ntop(AF_INET, &response.pnu.publicaddress.addr, external_address,
                 sizeof(external_address))) {
    return {false,
            {},
            "converting the NAT-PMP external IPv4 address failed: " +
                format_socket_error(last_socket_error()),
            {}};
  }
  return {true, external_address, {}, {}};
}
}  // namespace

std::string mp_describe_upnp_result(int result) {
  const char* description = strupnperror(result);
  std::string message = result > 0 ? "UPnP error " : "MiniUPnPc error ";
  message += std::to_string(result);
  if (description && *description) {
    message += " (" + std::string(description) + ")";
  }
  return message;
}

std::string mp_describe_natpmp_result(int result) {
  const char* description = strnatpmperr(result);
  switch (result) {
    case NATPMP_ERR_NOTAUTHORIZED:
      return "NAT-PMP request was not authorized or was refused (error " + std::to_string(result) +
             ")";
    case NATPMP_ERR_NETWORKFAILURE:
      return "router reported a NAT-PMP network failure (error " + std::to_string(result) + ")";
    case NATPMP_ERR_OUTOFRESOURCES:
      return "router has insufficient resources for the NAT-PMP mapping (error " +
             std::to_string(result) + ")";
    case NATPMP_ERR_UNSUPPORTEDVERSION:
      return "router returned an unsupported NAT-PMP version (error " + std::to_string(result) +
             ")";
    case NATPMP_ERR_UNSUPPORTEDOPCODE:
      return "router returned an unsupported NAT-PMP operation (error " + std::to_string(result) +
             ")";
    default:
      break;
  }
  std::string message = "libnatpmp error " + std::to_string(result);
  if (description && *description) {
    message += " (" + std::string(description) + ")";
  }
  return message;
}

bool mp_is_public_ipv4(const std::string& address) {
  uint32_t ip = 0;
  if (!parse_ipv4_host_order(address, ip)) {
    return false;
  }
  const uint8_t first = static_cast<uint8_t>(ip >> 24);
  const uint8_t second = static_cast<uint8_t>(ip >> 16);
  const uint8_t third = static_cast<uint8_t>(ip >> 8);
  if (first == 0 || first == 10 || first == 127 || first >= 224) {
    return false;
  }
  if (first == 100 && second >= 64 && second <= 127) {
    return false;
  }
  if (first == 169 && second == 254) {
    return false;
  }
  if (first == 172 && second >= 16 && second <= 31) {
    return false;
  }
  if (first == 192 && second == 168) {
    return false;
  }
  if (first == 192 && second == 0 && (third == 0 || third == 2)) {
    return false;
  }
  if (first == 198 && (second == 18 || second == 19)) {
    return false;
  }
  if ((first == 198 && second == 51 && third == 100) ||
      (first == 203 && second == 0 && third == 113)) {
    return false;
  }
  return ip != UINT32_C(0xffffffff);
}

MPPortMappingResult mp_open_udp_port_mapping(uint16_t local_port, uint16_t external_port) {
  std::string socket_error;
  if (!initialize_socket_runtime(socket_error)) {
    return {false, MPPortMappingMethod::NONE, {}, std::move(socket_error), {}};
  }

  NetworkAdapterInfo route;
  std::string route_error;
  if (!mp_find_preferred_ipv4_route(route, route_error)) {
    return {false,
            MPPortMappingMethod::NONE,
            {},
            "network route discovery failed before UPnP IGD or NAT-PMP could be attempted: " +
                route_error,
            {}};
  }
  log_port_mapping_route(route, true);

  auto upnp = upnp_add_mapping(route, local_port, external_port);
  if (upnp.success) {
    return {true, MPPortMappingMethod::UPNP_IGD, std::move(upnp.external_ip), std::move(upnp.error),
            std::move(upnp.context)};
  }

  auto natpmp = natpmp_request_mapping(route, local_port, external_port, kPortMappingLeaseSeconds);
  if (natpmp.success) {
    auto context = std::make_shared<MPPortMappingContext>();
    context->method = MPPortMappingMethod::NAT_PMP;
    context->route = route;
    auto external_address = natpmp_query_external_ip(route);
    std::string discovered_external_ip = std::move(external_address.external_ip);
    if (!mp_is_public_ipv4(discovered_external_ip) && mp_is_public_ipv4(upnp.external_ip)) {
      discovered_external_ip = std::move(upnp.external_ip);
    }
    return {true, MPPortMappingMethod::NAT_PMP, std::move(discovered_external_ip),
            external_address.success
                ? std::string()
                : "mapping succeeded, but querying the external IPv4 address failed: " +
                      external_address.error,
            std::move(context)};
  }

  const std::string upnp_error = upnp.error.empty() ? "unknown failure" : std::move(upnp.error);
  const std::string natpmp_error =
      natpmp.error.empty() ? "unknown failure" : std::move(natpmp.error);
  return {false,
          MPPortMappingMethod::NONE,
          mp_is_public_ipv4(upnp.external_ip) ? std::move(upnp.external_ip) : std::string(),
          "UPnP IGD: " + upnp_error + "; NAT-PMP: " + natpmp_error,
          {}};
}

MPPortMappingResult mp_refresh_udp_port_mapping(const MPPortMappingResult& mapping,
                                                uint16_t local_port,
                                                uint16_t external_port) {
  if (!mapping.context || mapping.context->method != mapping.method) {
    return {false, mapping.method, {}, "active port-mapping context is missing or invalid", {}};
  }
  if (mapping.method == MPPortMappingMethod::UPNP_IGD) {
    return {true, mapping.method, {}, {}, mapping.context};
  }
  if (mapping.method != MPPortMappingMethod::NAT_PMP) {
    return {false, mapping.method, {}, "no active port-mapping method", mapping.context};
  }

  NetworkAdapterInfo current_route;
  std::string route_error;
  if (!mp_find_preferred_ipv4_route(current_route, route_error)) {
    return {false,
            mapping.method,
            {},
            "network route discovery failed during NAT-PMP refresh: " + route_error,
            mapping.context};
  }
  log_port_mapping_route(current_route, false);
  if (!mp_same_port_mapping_route(mapping.context->route, current_route)) {
    return {false,
            mapping.method,
            {},
            "preferred network route changed from " +
                mp_format_port_mapping_route(mapping.context->route) + " to " +
                mp_format_port_mapping_route(current_route) +
                "; retry multiplayer setup to map the new route",
            mapping.context};
  }

  auto refresh = natpmp_request_mapping(mapping.context->route, local_port, external_port,
                                        kPortMappingLeaseSeconds);
  return {refresh.success, mapping.method, {}, std::move(refresh.error), mapping.context};
}

void mp_close_udp_port_mapping(const MPPortMappingResult& mapping,
                               uint16_t local_port,
                               uint16_t external_port) {
  if (!mapping.context || mapping.context->method != mapping.method) {
    lg::debug("[Multiplayer] Port-mapping cleanup skipped because its context is missing.");
    return;
  }
  log_port_mapping_route(mapping.context->route, false);
  switch (mapping.method) {
    case MPPortMappingMethod::UPNP_IGD:
      upnp_delete_mapping(*mapping.context, external_port);
      break;
    case MPPortMappingMethod::NAT_PMP: {
      auto cleanup = natpmp_request_mapping(mapping.context->route, local_port, external_port, 0);
      if (!cleanup.success) {
        lg::debug("[Multiplayer] NAT-PMP cleanup for UDP port {} failed: {}.", external_port,
                  cleanup.error.empty() ? "unknown failure" : cleanup.error);
      }
      break;
    }
    case MPPortMappingMethod::NONE:
      break;
  }
}
