#include "game/multiplayer/multiplayer_port_mapping.h"

#include "common/log/log.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#include <natupnp.h>
#include <objbase.h>
#include <oleauto.h>

#include <array>
#include <string>
#endif

namespace {
constexpr uint32_t kPortMappingLeaseSeconds = 7200;

#ifdef _WIN32
struct NetworkAdapterInfo {
  std::string local_ip;
  sockaddr_in gateway = {};
};

uint16_t read_be16(const uint8_t* data) {
  return (uint16_t)((data[0] << 8) | data[1]);
}

void write_be16(uint8_t* data, uint16_t value) {
  data[0] = (uint8_t)((value >> 8) & 0xff);
  data[1] = (uint8_t)(value & 0xff);
}

void write_be32(uint8_t* data, uint32_t value) {
  data[0] = (uint8_t)((value >> 24) & 0xff);
  data[1] = (uint8_t)((value >> 16) & 0xff);
  data[2] = (uint8_t)((value >> 8) & 0xff);
  data[3] = (uint8_t)(value & 0xff);
}

bool find_primary_adapter(NetworkAdapterInfo& out) {
  ULONG buffer_size = 15 * 1024;
  std::array<uint8_t, 15 * 1024> stack_buffer = {};
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(stack_buffer.data());

  ULONG result = GetAdaptersAddresses(AF_INET,
                                      GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                          GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, adapters, &buffer_size);
  if (result != NO_ERROR) {
    return false;
  }

  for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
        !adapter->FirstGatewayAddress || !adapter->FirstUnicastAddress) {
      continue;
    }

    sockaddr_in gateway = {};
    for (auto* gateway_addr = adapter->FirstGatewayAddress; gateway_addr;
         gateway_addr = gateway_addr->Next) {
      if (gateway_addr->Address.lpSockaddr &&
          gateway_addr->Address.lpSockaddr->sa_family == AF_INET) {
        gateway = *reinterpret_cast<sockaddr_in*>(gateway_addr->Address.lpSockaddr);
        break;
      }
    }
    if (gateway.sin_addr.s_addr == 0) {
      continue;
    }

    for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
      if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }

      char local_ip[INET_ADDRSTRLEN] = {};
      auto* local_addr = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
      if (!inet_ntop(AF_INET, &local_addr->sin_addr, local_ip, sizeof(local_ip))) {
        continue;
      }

      out.local_ip = local_ip;
      out.gateway = gateway;
      return true;
    }
  }

  return false;
}

class ComApartment {
 public:
  ComApartment() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    initialized = SUCCEEDED(hr);
    already_initialized = hr == RPC_E_CHANGED_MODE;
  }

  ~ComApartment() {
    if (initialized && !already_initialized) {
      CoUninitialize();
    }
  }

  bool usable() const { return initialized || already_initialized; }

 private:
  bool initialized = false;
  bool already_initialized = false;
};

bool upnp_add_mapping(uint16_t local_port, uint16_t external_port) {
  NetworkAdapterInfo adapter;
  if (!find_primary_adapter(adapter)) {
    return false;
  }

  ComApartment com;
  if (!com.usable()) {
    return false;
  }

  IUPnPNAT* nat = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_UPnPNAT, nullptr, CLSCTX_INPROC_SERVER, IID_IUPnPNAT,
                                reinterpret_cast<void**>(&nat));
  if (FAILED(hr) || !nat) {
    return false;
  }

  IStaticPortMappingCollection* mappings = nullptr;
  hr = nat->get_StaticPortMappingCollection(&mappings);
  nat->Release();
  if (FAILED(hr) || !mappings) {
    return false;
  }

  BSTR protocol = SysAllocString(L"UDP");
  std::wstring local_ip(adapter.local_ip.begin(), adapter.local_ip.end());
  BSTR local_client = SysAllocString(local_ip.c_str());
  BSTR description = SysAllocString(L"OpenGOAL Jak II Multiplayer");
  IStaticPortMapping* mapping = nullptr;

  hr = mappings->Add(external_port, protocol, local_port, local_client, VARIANT_TRUE, description,
                     &mapping);

  if (mapping) {
    mapping->Release();
  }
  SysFreeString(description);
  SysFreeString(local_client);
  SysFreeString(protocol);
  mappings->Release();

  return SUCCEEDED(hr);
}

void upnp_delete_mapping(uint16_t external_port) {
  ComApartment com;
  if (!com.usable()) {
    return;
  }

  IUPnPNAT* nat = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_UPnPNAT, nullptr, CLSCTX_INPROC_SERVER, IID_IUPnPNAT,
                                reinterpret_cast<void**>(&nat));
  if (FAILED(hr) || !nat) {
    return;
  }

  IStaticPortMappingCollection* mappings = nullptr;
  hr = nat->get_StaticPortMappingCollection(&mappings);
  nat->Release();
  if (FAILED(hr) || !mappings) {
    return;
  }

  BSTR protocol = SysAllocString(L"UDP");
  mappings->Remove(external_port, protocol);
  SysFreeString(protocol);
  mappings->Release();
}

bool natpmp_request_mapping(uint16_t local_port, uint16_t external_port, uint32_t lifetime_seconds) {
  NetworkAdapterInfo adapter;
  if (!find_primary_adapter(adapter)) {
    return false;
  }

  SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) {
    return false;
  }

  DWORD timeout_ms = 500;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
             sizeof(timeout_ms));

  sockaddr_in gateway = adapter.gateway;
  gateway.sin_port = htons(5351);

  uint8_t request[12] = {};
  request[0] = 0;  // NAT-PMP version.
  request[1] = 1;  // Map UDP.
  write_be16(&request[4], local_port);
  write_be16(&request[6], external_port);
  write_be32(&request[8], lifetime_seconds);

  uint8_t response[16] = {};
  bool success = false;
  for (int attempt = 0; attempt < 3 && !success; ++attempt) {
    const int sent = sendto(sock, reinterpret_cast<const char*>(request), sizeof(request), 0,
                            reinterpret_cast<sockaddr*>(&gateway), sizeof(gateway));
    if (sent != sizeof(request)) {
      continue;
    }

    sockaddr_in from = {};
    int from_len = sizeof(from);
    const int received = recvfrom(sock, reinterpret_cast<char*>(response), sizeof(response), 0,
                                  reinterpret_cast<sockaddr*>(&from), &from_len);
    if (received < 16 || from.sin_addr.s_addr != gateway.sin_addr.s_addr) {
      continue;
    }

    const uint16_t result_code = read_be16(&response[2]);
    success = response[0] == 0 && response[1] == 129 && result_code == 0;
  }

  closesocket(sock);
  return success;
}
#endif
}  // namespace

MPPortMappingResult mp_open_udp_port_mapping(uint16_t local_port, uint16_t external_port) {
#ifdef _WIN32
  WSADATA wsa_data = {};
  WSAStartup(MAKEWORD(2, 2), &wsa_data);

  if (upnp_add_mapping(local_port, external_port)) {
    return {true, MPPortMappingMethod::UPNP_IGD, ""};
  }

  if (natpmp_request_mapping(local_port, external_port, kPortMappingLeaseSeconds)) {
    return {true, MPPortMappingMethod::NAT_PMP, ""};
  }

  return {false, MPPortMappingMethod::NONE, "router did not accept UPnP IGD or NAT-PMP mapping"};
#else
  (void)local_port;
  (void)external_port;
  return {false, MPPortMappingMethod::NONE, "automatic port mapping is only implemented on Windows"};
#endif
}

bool mp_refresh_udp_port_mapping(MPPortMappingMethod method,
                                 uint16_t local_port,
                                 uint16_t external_port) {
#ifdef _WIN32
  switch (method) {
    case MPPortMappingMethod::UPNP_IGD:
      return true;
    case MPPortMappingMethod::NAT_PMP:
      return natpmp_request_mapping(local_port, external_port, kPortMappingLeaseSeconds);
    case MPPortMappingMethod::NONE:
      return false;
  }
  return false;
#else
  (void)method;
  (void)local_port;
  (void)external_port;
  return false;
#endif
}

void mp_close_udp_port_mapping(MPPortMappingMethod method,
                               uint16_t local_port,
                               uint16_t external_port) {
#ifdef _WIN32
  switch (method) {
    case MPPortMappingMethod::UPNP_IGD:
      upnp_delete_mapping(external_port);
      break;
    case MPPortMappingMethod::NAT_PMP:
      natpmp_request_mapping(local_port, external_port, 0);
      break;
    case MPPortMappingMethod::NONE:
      break;
  }
#else
  (void)method;
  (void)local_port;
  (void)external_port;
#endif
}
