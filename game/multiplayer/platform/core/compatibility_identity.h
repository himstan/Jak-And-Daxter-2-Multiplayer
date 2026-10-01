#pragma once

#include <string>
#include <string_view>

namespace multiplayer::platform {

inline constexpr size_t kCompatibilityIdentityMaxLength = 64;
inline constexpr std::string_view kCompatibilityIdentityPlaceholder = "%MODVERSIONPLACEHOLDER%";

bool canonicalize_semver(std::string_view version, std::string& canonical);
bool valid_compatibility_identity(std::string_view identity);
bool resolve_compatibility_identity(std::string_view configured_version,
                                    std::string_view commit_sha,
                                    std::string& identity);

}  // namespace multiplayer::platform
