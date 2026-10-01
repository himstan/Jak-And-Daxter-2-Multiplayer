#include "game/multiplayer/platform/nat/port_mapping.h"

#include "common/log/log.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WS2tcpip.h>
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#endif

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "miniupnpc.h"
#include "natpmp.h"
#include "upnpcommands.h"
#include "upnperrors.h"

struct MPPortMappingContext {
  MPPortMappingMethod method = MPPortMappingMethod::NONE;
  std::string upnp_control_url;
  std::string upnp_service_type;
};

namespace {
constexpr uint32_t kPortMappingLeaseSeconds = 7200;
constexpr int kUpnpDiscoveryDelayMilliseconds = 2000;
constexpr auto kNatPmpOperationDeadline = std::chrono::milliseconds(2500);

struct AttemptResult {
  bool success = false;
  std::string external_ip;
  std::string error;
  std::shared_ptr<MPPortMappingContext> context;
};

std::string upnp_error(const int result) {
  const char* text = strupnperror(result);
  return text && *text ? std::string(text) + " (" + std::to_string(result) + ")"
                       : "UPnP error " + std::to_string(result);
}

std::string natpmp_error(const int result) {
  const char* text = strnatpmperr(result);
  return text && *text ? std::string(text) + " (" + std::to_string(result) + ")"
                       : "NAT-PMP error " + std::to_string(result);
}

#ifdef _WIN32
bool initialize_socket_runtime(std::string& error) {
  static std::once_flag once;
  static int startup_result = WSASYSNOTREADY;
  std::call_once(once, [] {
    WSADATA data{};
    startup_result = WSAStartup(MAKEWORD(2, 2), &data);
  });

  if (startup_result != 0) {
    error = "Winsock initialization failed (" + std::to_string(startup_result) + ")";
    return false;
  }
  return true;
}
#else
bool initialize_socket_runtime(std::string&) {
  return true;
}
#endif

struct UpnpDeviceListDeleter {
  void operator()(UPNPDev* devices) const { freeUPNPDevlist(devices); }
};

class UpnpUrls {
 public:
  ~UpnpUrls() { FreeUPNPUrls(&value_); }
  UPNPUrls* get() { return &value_; }

 private:
  UPNPUrls value_{};
};

std::string describe_status(const int status) {
  switch (status) {
    case UPNP_NO_IGD:
      return "no UPnP IGD found";
    case UPNP_DISCONNECTED_IGD:
      return "UPnP IGD is disconnected";
    case UPNP_UNKNOWN_DEVICE:
      return "UPnP device is not a recognized IGD";
    default:
      return "unexpected UPnP IGD status " + std::to_string(status);
  }
}

AttemptResult add_mapping(const uint16_t local_port, const uint16_t external_port) {
  int discovery_error = UPNPDISCOVER_UNKNOWN_ERROR;
  const std::unique_ptr<UPNPDev, UpnpDeviceListDeleter> devices(
      upnpDiscover(kUpnpDiscoveryDelayMilliseconds, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2,
                   &discovery_error));
  if (!devices) {
    return {.error = "UPnP discovery failed (" + std::to_string(discovery_error) + ")"};
  }

  UpnpUrls urls;
  IGDdatas data{};
  char local_address[64]{};
  char external_address[64]{};
  const int status =
      UPNP_GetValidIGD(devices.get(), urls.get(), &data, local_address, sizeof(local_address),
                       external_address, sizeof(external_address));
  if (status != UPNP_CONNECTED_IGD && status != UPNP_PRIVATEIP_IGD) {
    return {.external_ip = external_address, .error = describe_status(status)};
  }

  if (!urls.get()->controlURL || !*urls.get()->controlURL || !*data.first.servicetype ||
      !*local_address) {
    return {.external_ip = external_address, .error = "UPnP IGD returned incomplete mapping data"};
  }

  const std::string local_port_text = std::to_string(local_port);
  const std::string external_port_text = std::to_string(external_port);
  const int result = UPNP_AddPortMapping(
      urls.get()->controlURL, data.first.servicetype, external_port_text.c_str(),
      local_port_text.c_str(), local_address, "OpenGOAL Jak II Multiplayer", "UDP", "", "0");
  if (result != UPNPCOMMAND_SUCCESS) {
    return {.external_ip = external_address, .error = "UPnP mapping failed: " + upnp_error(result)};
  }

  auto context = std::make_shared<MPPortMappingContext>();
  context->method = MPPortMappingMethod::UPNP_IGD;
  context->upnp_control_url = urls.get()->controlURL;
  context->upnp_service_type = data.first.servicetype;

  return {.success = true, .external_ip = external_address, .context = std::move(context)};
}

void delete_mapping(const MPPortMappingContext& context, const uint16_t external_port) {
  if (context.upnp_control_url.empty() || context.upnp_service_type.empty()) {
    return;
  }

  const std::string port = std::to_string(external_port);
  const int result = UPNP_DeletePortMapping(
      context.upnp_control_url.c_str(), context.upnp_service_type.c_str(), port.c_str(), "UDP", "");
  if (result != UPNPCOMMAND_SUCCESS) {
    lg::debug("[Multiplayer] UPnP cleanup for UDP port {} failed: {}.", external_port,
              upnp_error(result));
  }
}

class NatPmpClient {
 public:
  ~NatPmpClient() {
    if (open_) {
      (void)closenatpmp(&client_);
    }
  }

  bool open(std::string& error) {
    if (const int result = initnatpmp(&client_, 0, 0); result != 0) {
      if (result != NATPMP_ERR_SOCKETERROR && result != NATPMP_ERR_INVALIDARGS) {
        (void)closenatpmp(&client_);
      }
      error = natpmp_error(result);
      return false;
    }
    open_ = true;
    return true;
  }

  natpmp_t* get() { return &client_; }

 private:
  natpmp_t client_{};
  bool open_ = false;
};

bool wait_for_response(NatPmpClient& client, natpmpresp_t& response, std::string& error) {
  const auto deadline = std::chrono::steady_clock::now() + kNatPmpOperationDeadline;

  while (std::chrono::steady_clock::now() < deadline) {
    const int result = readnatpmpresponseorretry(client.get(), &response);
    if (result == 0) {
      return true;
    }
    if (result != NATPMP_TRYAGAIN) {
      error = natpmp_error(result);
      return false;
    }

    timeval timeout{};
    if (const int timeout_result = getnatpmprequesttimeout(client.get(), &timeout);
        timeout_result < 0) {
      error = natpmp_error(timeout_result);
      return false;
    }

    auto delay = std::chrono::seconds(std::max<long>(timeout.tv_sec, 0)) +
                 std::chrono::microseconds(std::max<long>(timeout.tv_usec, 0));
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      break;
    }
    const auto remaining_us = std::chrono::duration_cast<std::chrono::microseconds>(remaining);
    std::this_thread::sleep_for(std::min(delay, remaining_us));
  }

  error = "NAT-PMP response timed out";
  return false;
}

AttemptResult request_mapping(const uint16_t local_port,
                              const uint16_t external_port,
                              const uint32_t lifetime_seconds) {
  NatPmpClient client;
  if (std::string error; !client.open(error)) {
    return {.error = "NAT-PMP initialization failed: " + error};
  }

  const int send_result = sendnewportmappingrequest(client.get(), NATPMP_PROTOCOL_UDP, local_port,
                                                    external_port, lifetime_seconds);
  if (send_result < 0) {
    return {.error = "NAT-PMP mapping request failed: " + natpmp_error(send_result)};
  }

  natpmpresp_t response{};
  if (std::string error; !wait_for_response(client, response, error)) {
    return {.error = "NAT-PMP mapping response failed: " + error};
  }
  if (response.type != NATPMP_RESPTYPE_UDPPORTMAPPING) {
    return {.error = "NAT-PMP returned an unexpected response type"};
  }

  if (lifetime_seconds != 0 && response.pnu.newportmapping.mappedpublicport != external_port) {
    (void)request_mapping(local_port, 0, 0);
    return {.error = "NAT-PMP assigned UDP port " +
                     std::to_string(response.pnu.newportmapping.mappedpublicport) + " instead of " +
                     std::to_string(external_port)};
  }

  return {.success = true};
}

AttemptResult query_external_ip() {
  NatPmpClient client;
  if (std::string error; !client.open(error)) {
    return {.error = "NAT-PMP initialization failed: " + error};
  }

  if (const int result = sendpublicaddressrequest(client.get()); result < 0) {
    return {.error = "NAT-PMP external-address request failed: " + natpmp_error(result)};
  }

  natpmpresp_t response{};
  if (std::string error; !wait_for_response(client, response, error)) {
    return {.error = "NAT-PMP external-address response failed: " + error};
  }
  if (response.type != NATPMP_RESPTYPE_PUBLICADDRESS) {
    return {.error = "NAT-PMP returned an unexpected external-address response"};
  }

  char address[INET_ADDRSTRLEN]{};
  if (!inet_ntop(AF_INET, &response.pnu.publicaddress.addr, address, sizeof(address))) {
    return {.error = "failed to format NAT-PMP external IPv4 address"};
  }

  return {.success = true, .external_ip = address};
}

}  // namespace

bool is_private_ipv4(const std::string& address) {
  in_addr addr{};
  if (inet_pton(AF_INET, address.c_str(), &addr) != 1)
    return true;
  const uint32_t ip = ntohl(addr.s_addr);
  return (ip & 0xff000000) == 0x0a000000 || (ip & 0xfff00000) == 0xac100000 ||
         (ip & 0xffff0000) == 0xc0a80000 || (ip & 0xffc00000) == 0x64400000;
}

MPPortMappingResult open_port_mapping(const uint16_t local_port, const uint16_t external_port) {
  if (std::string error; !initialize_socket_runtime(error)) {
    return {.error = std::move(error)};
  }

  auto [success, external_ip, error, context] = add_mapping(local_port, external_port);
  if (success) {
    return {.success = true,
            .method = MPPortMappingMethod::UPNP_IGD,
            .external_ip = std::move(external_ip),
            .context = std::move(context)};
  }

  auto natpmp = request_mapping(local_port, external_port, kPortMappingLeaseSeconds);
  if (natpmp.success) {
    auto mapping_context = std::make_shared<MPPortMappingContext>();
    mapping_context->method = MPPortMappingMethod::NAT_PMP;

    auto address = query_external_ip();
    return {.success = true,
            .method = MPPortMappingMethod::NAT_PMP,
            .external_ip = std::move(address.external_ip),
            .error = address.success ? std::string{} : std::move(address.error),
            .context = std::move(mapping_context)};
  }

  return {.error = "UPnP: " + (error.empty() ? "unknown failure" : error) +
                   "; NAT-PMP: " + (natpmp.error.empty() ? "unknown failure" : natpmp.error)};
}

MPPortMappingResult refresh_udp_port_mapping(const MPPortMappingResult& mapping,
                                             const uint16_t local_port,
                                             const uint16_t external_port) {
  if (!mapping.context || mapping.context->method != mapping.method) {
    return {.method = mapping.method, .error = "active port-mapping context is missing"};
  }

  if (mapping.method == MPPortMappingMethod::UPNP_IGD) {
    return {.success = true, .method = mapping.method, .context = mapping.context};
  }

  if (mapping.method != MPPortMappingMethod::NAT_PMP) {
    return {.method = mapping.method, .error = "no active port-mapping method"};
  }

  auto refresh = request_mapping(local_port, external_port, kPortMappingLeaseSeconds);
  return {.success = refresh.success,
          .method = mapping.method,
          .error = std::move(refresh.error),
          .context = mapping.context};
}

void close_udp_port_mapping(const MPPortMappingResult& mapping,
                            const uint16_t local_port,
                            const uint16_t external_port) {
  if (!mapping.context || mapping.context->method != mapping.method) {
    return;
  }

  if (mapping.method == MPPortMappingMethod::UPNP_IGD) {
    delete_mapping(*mapping.context, external_port);
    return;
  }

  if (mapping.method == MPPortMappingMethod::NAT_PMP) {
    if (auto result = request_mapping(local_port, 0, 0); !result.success) {
      lg::debug("[Multiplayer] NAT-PMP cleanup for UDP port {} failed: {}.", external_port,
                result.error.empty() ? "unknown failure" : result.error);
    }
  }
}
