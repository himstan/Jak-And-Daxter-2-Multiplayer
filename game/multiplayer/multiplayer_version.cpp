#include "game/multiplayer/multiplayer_version.h"

namespace {
bool ascii_alphanumeric(char value) {
  return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z');
}

bool valid_numeric_identifier(std::string_view identifier) {
  if (identifier.empty() || (identifier.size() > 1 && identifier.front() == '0')) {
    return false;
  }
  for (char value : identifier) {
    if (value < '0' || value > '9') {
      return false;
    }
  }
  return true;
}

bool valid_identifier_list(std::string_view identifiers, bool reject_numeric_leading_zero) {
  if (identifiers.empty()) {
    return false;
  }
  size_t start = 0;
  while (start < identifiers.size()) {
    const size_t end = identifiers.find('.', start);
    const size_t length = (end == std::string_view::npos ? identifiers.size() : end) - start;
    const std::string_view identifier = identifiers.substr(start, length);
    if (identifier.empty()) {
      return false;
    }
    bool numeric = true;
    for (char value : identifier) {
      if (!ascii_alphanumeric(value) && value != '-') {
        return false;
      }
      numeric = numeric && value >= '0' && value <= '9';
    }
    if (reject_numeric_leading_zero && numeric && identifier.size() > 1 &&
        identifier.front() == '0') {
      return false;
    }
    if (end == std::string_view::npos) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

bool valid_commit_sha(std::string_view commit_sha) {
  if (commit_sha.size() < 7 || commit_sha.size() > 40) {
    return false;
  }
  for (char value : commit_sha) {
    if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
          (value >= 'A' && value <= 'F'))) {
      return false;
    }
  }
  return true;
}
}  // namespace

bool mp_canonicalize_semver(std::string_view version, std::string& canonical) {
  canonical.clear();
  if (!version.empty() && version.front() == 'v') {
    version.remove_prefix(1);
  }
  if (version.empty() || version.size() + 1 > kMultiplayerVersionMaxLength) {
    return false;
  }

  const size_t build_separator = version.find('+');
  if (build_separator != std::string_view::npos &&
      version.find('+', build_separator + 1) != std::string_view::npos) {
    return false;
  }
  const std::string_view before_build = version.substr(0, build_separator);
  const std::string_view build = build_separator == std::string_view::npos
                                     ? std::string_view{}
                                     : version.substr(build_separator + 1);
  if (build_separator != std::string_view::npos && !valid_identifier_list(build, false)) {
    return false;
  }

  const size_t prerelease_separator = before_build.find('-');
  const std::string_view core = before_build.substr(0, prerelease_separator);
  const std::string_view prerelease = prerelease_separator == std::string_view::npos
                                          ? std::string_view{}
                                          : before_build.substr(prerelease_separator + 1);
  if (prerelease_separator != std::string_view::npos && !valid_identifier_list(prerelease, true)) {
    return false;
  }

  size_t start = 0;
  for (int component = 0; component < 3; ++component) {
    const size_t end = component == 2 ? core.size() : core.find('.', start);
    if (end == std::string_view::npos ||
        !valid_numeric_identifier(core.substr(start, end - start))) {
      return false;
    }
    start = end + 1;
  }
  if (start != core.size() + 1) {
    return false;
  }

  canonical.reserve(version.size() + 1);
  canonical.push_back('v');
  canonical.append(version);
  return true;
}

bool mp_valid_compatibility_identity(std::string_view identity) {
  if (identity.starts_with("dev-")) {
    return valid_commit_sha(identity.substr(4));
  }
  std::string canonical;
  return mp_canonicalize_semver(identity, canonical) && canonical == identity;
}

bool mp_resolve_compatibility_identity(std::string_view configured_version,
                                       std::string_view commit_sha,
                                       std::string& identity) {
  identity.clear();
  if (configured_version == kMultiplayerVersionPlaceholder) {
    if (!valid_commit_sha(commit_sha) || commit_sha.size() + 4 > kMultiplayerVersionMaxLength) {
      return false;
    }
    identity = "dev-";
    identity.append(commit_sha);
    return true;
  }
  return mp_canonicalize_semver(configured_version, identity);
}
