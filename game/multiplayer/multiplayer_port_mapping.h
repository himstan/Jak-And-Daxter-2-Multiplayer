#pragma once

#include <cstdint>

enum class MPPortMappingMethod {
  NONE,
  UPNP_IGD,
  NAT_PMP,
};

struct MPPortMappingResult {
  bool success = false;
  MPPortMappingMethod method = MPPortMappingMethod::NONE;
  const char* error = "";
};

MPPortMappingResult mp_open_udp_port_mapping(uint16_t local_port, uint16_t external_port);
bool mp_refresh_udp_port_mapping(MPPortMappingMethod method,
                                 uint16_t local_port,
                                 uint16_t external_port);
void mp_close_udp_port_mapping(MPPortMappingMethod method,
                               uint16_t local_port,
                               uint16_t external_port);
