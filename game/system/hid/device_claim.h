#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

class DeviceClaim {
 public:
  static std::unique_ptr<DeviceClaim> acquire(std::string_view key);
  ~DeviceClaim();
  DeviceClaim(const DeviceClaim&) = delete;
  DeviceClaim& operator=(const DeviceClaim&) = delete;

 private:
  explicit DeviceClaim(intptr_t handle) : m_handle(handle) {}
  intptr_t m_handle;
};
