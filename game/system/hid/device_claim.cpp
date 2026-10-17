#include "device_claim.h"

#ifdef _WIN32
#include <windows.h>

#include "common/common_types.h"
#include "common/log/log.h"

#include "fmt/format.h"

namespace {
u64 stable_claim_hash(const std::string& claim_key) {
  u64 hash = 14695981039346656037ull;
  for (const auto character : claim_key) {
    hash ^= (u8)character;
    hash *= 1099511628211ull;
  }
  return hash;
}
}  // namespace
#endif

std::optional<void*> claim_device(const std::string& claim_key) {
#ifdef _WIN32
  const auto mutex_name =
      fmt::format("Local\\OpenGOALControllerClaim_{:016x}", stable_claim_hash(claim_key));
  const HANDLE claim_mutex = CreateMutexA(nullptr, false, mutex_name.c_str());
  if (!claim_mutex) {
    lg::warn("Unable to create device claim mutex for {}; allowing device assignment", claim_key);
    return nullptr;
  }
  if (const auto wait_result = WaitForSingleObject(claim_mutex, 0);
      wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
    return claim_mutex;
  }
  CloseHandle(claim_mutex);
  return std::nullopt;
#else
  (void)claim_key;
  return nullptr;
#endif
}

void release_device(void* claim) {
#ifdef _WIN32
  if (claim) {
    ReleaseMutex((HANDLE)claim);
    CloseHandle((HANDLE)claim);
  }
#else
  (void)claim;
#endif
}
