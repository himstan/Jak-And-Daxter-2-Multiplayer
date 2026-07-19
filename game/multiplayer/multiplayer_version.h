#pragma once

#include <cstddef>
#include <string>
#include <string_view>

constexpr size_t kMultiplayerVersionMaxLength = 64;
constexpr std::string_view kMultiplayerVersionPlaceholder = "%MODVERSIONPLACEHOLDER%";

bool mp_canonicalize_semver(std::string_view version, std::string& canonical);
bool mp_valid_compatibility_identity(std::string_view identity);
bool mp_resolve_compatibility_identity(std::string_view configured_version,
                                       std::string_view commit_sha,
                                       std::string& identity);
