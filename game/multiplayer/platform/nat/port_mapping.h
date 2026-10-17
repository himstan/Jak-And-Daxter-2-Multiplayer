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
  uint16_t external_port = 0;
  std::string external_ip;
  std::string error;
  std::shared_ptr<MPPortMappingContext> context;
};

MPPortMappingResult open_port_mapping(uint16_t local_port, uint16_t preferred_port);
bool is_private_ipv4(const std::string& address);
std::string refresh_udp_port_mapping(const MPPortMappingResult& mapping, uint16_t local_port);
void close_udp_port_mapping(const MPPortMappingResult& mapping, uint16_t local_port);
