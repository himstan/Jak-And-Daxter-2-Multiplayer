#include "device_claim.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <sys/file.h>
#endif

#include "common/util/FileUtil.h"

#include "fmt/format.h"

std::unique_ptr<DeviceClaim> DeviceClaim::acquire(const std::string_view key) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char character : key) {
    hash = (hash ^ character) * 1099511628211ull;
  }
#ifdef _WIN32
  const auto name = fmt::format("Local\\OpenGOALControllerClaim_{:016x}", hash);
  const HANDLE handle = CreateMutexA(nullptr, false, name.c_str());
  if (!handle) {
    return nullptr;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(handle);
    return nullptr;
  }
  return std::unique_ptr<DeviceClaim>(new DeviceClaim(reinterpret_cast<intptr_t>(handle)));
#else
  const auto path = fs::temp_directory_path() /
                    fmt::format("opengoal-controller-{}-{:016x}.lock", getuid(), hash);
  const int handle = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (handle < 0) {
    return nullptr;
  }
  if (flock(handle, LOCK_EX | LOCK_NB) != 0) {
    close(handle);
    return nullptr;
  }
  return std::unique_ptr<DeviceClaim>(new DeviceClaim(handle));
#endif
}

DeviceClaim::~DeviceClaim() {
#ifdef _WIN32
  CloseHandle(reinterpret_cast<HANDLE>(m_handle));
#else
  close(static_cast<int>(m_handle));
#endif
}
