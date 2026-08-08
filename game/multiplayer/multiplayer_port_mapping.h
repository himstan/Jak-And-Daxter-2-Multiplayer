#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct MPPortMappingContext;

enum class MPPortMappingMethod {
  NONE,
  UPNP_IGD,
  NAT_PMP,
};

struct MPPortMappingResult {
  bool success = false;
  MPPortMappingMethod method = MPPortMappingMethod::NONE;
  std::string external_ip;
  std::string error;
  std::shared_ptr<MPPortMappingContext> context;
};

enum class MPPortMappingState {
  IDLE,
  PENDING,
  READY,
  FAILED,
};

MPPortMappingResult mp_open_udp_port_mapping(uint16_t local_port, uint16_t external_port);
bool mp_is_public_ipv4(const std::string& address);
MPPortMappingResult mp_refresh_udp_port_mapping(const MPPortMappingResult& mapping,
                                                uint16_t local_port,
                                                uint16_t external_port);
void mp_close_udp_port_mapping(const MPPortMappingResult& mapping,
                               uint16_t local_port,
                               uint16_t external_port);
