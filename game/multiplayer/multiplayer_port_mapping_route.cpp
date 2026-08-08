#include "game/multiplayer/multiplayer_port_mapping_route.h"

#include <string>

namespace {
std::string format_metric(const std::optional<uint32_t>& metric) {
  return metric ? std::to_string(*metric) : "n/a";
}
}  // namespace

std::string mp_format_port_mapping_route(const NetworkAdapterInfo& route) {
  return "adapter \"" + route.name + "\" (interface " + std::to_string(route.interface_index) +
         "), local " + route.local_ip + ", gateway " + route.gateway_ip +
         ", metrics route/interface " + format_metric(route.route_metric) + "/" +
         format_metric(route.interface_metric);
}

bool mp_same_port_mapping_route(const NetworkAdapterInfo& lhs, const NetworkAdapterInfo& rhs) {
  return lhs.interface_index == rhs.interface_index && lhs.local_ip == rhs.local_ip &&
         lhs.gateway_ip == rhs.gateway_ip;
}
