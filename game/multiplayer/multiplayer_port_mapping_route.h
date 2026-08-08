#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct NetworkAdapterInfo {
  std::string name;
  std::string local_ip;
  std::string gateway_ip;
  uint32_t interface_index = 0;
  std::optional<uint32_t> route_metric;
  std::optional<uint32_t> interface_metric;
};

bool mp_find_preferred_ipv4_route(NetworkAdapterInfo& route, std::string& error);
std::string mp_format_port_mapping_route(const NetworkAdapterInfo& route);
bool mp_same_port_mapping_route(const NetworkAdapterInfo& lhs, const NetworkAdapterInfo& rhs);
