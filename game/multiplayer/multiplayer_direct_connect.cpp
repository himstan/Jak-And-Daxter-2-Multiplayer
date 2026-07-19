#include "game/multiplayer/multiplayer_direct_connect.h"

#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>

#include <sodium.h>

#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_scanner.h"
#include "game/multiplayer/multiplayer_security.h"
#include "game/multiplayer/multiplayer_session.h"

#include "third-party/SDL/include/SDL3/SDL.h"

namespace {
constexpr int kAddressField = 0;
constexpr int kPortField = 1;
constexpr int kTokenField = 2;
constexpr std::string_view kDefaultPort = "26210";
constexpr const char* kValidationToken = "AAAAAAAAAAAAAAAAAAAAAA";

uint16_t port_value(const MultiplayerData& data) {
  const std::string_view text(data.direct_port.data());
  uint32_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size() ||
      value == 0 || value > (std::numeric_limits<uint16_t>::max)()) {
    return 0;
  }
  return static_cast<uint16_t>(value);
}

bool valid_parts(const std::string& address, uint16_t port, const std::string& token) {
  if (address.empty() || port == 0 || (!token.empty() && token.size() != 22)) {
    return false;
  }
  MultiplayerSecurity validator;
  std::string parsed_address;
  uint16_t parsed_port = 0;
  std::string invite = address + ":" + std::to_string(port) + "/" +
                       (token.empty() ? kValidationToken : token);
  const bool valid = validator.start_client(invite, parsed_address, parsed_port);
  mp_secure_clear_string(invite);
  return valid && parsed_address == address && parsed_port == port;
}

template <size_t Size>
void replace_field(std::array<char, Size>& destination, const std::string& value) {
  sodium_memzero(destination.data(), destination.size());
  memcpy(destination.data(), value.data(), value.size());
}

bool valid_clipboard_field(int field, const std::string& value) {
  if (field == kAddressField) {
    return valid_parts(value, 1, "");
  }
  if (field == kPortField) {
    if (value.empty() || value.size() > 5) {
      return false;
    }
    uint32_t port = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
    return result.ec == std::errc() && result.ptr == value.data() + value.size() && port > 0 &&
           port <= (std::numeric_limits<uint16_t>::max)();
  }
  return field == kTokenField && value.size() == 22 && valid_parts("127.0.0.1", 1, value);
}

bool paste_field(MultiplayerData& data, int field) {
  char* clipboard = SDL_GetClipboardText();
  if (!clipboard) {
    return false;
  }
  constexpr size_t kMaximumComponentSize = 22;
  size_t length = 0;
  while (length <= kMaximumComponentSize && clipboard[length] != '\0') {
    ++length;
  }
  const bool bounded = length <= kMaximumComponentSize;
  std::string value;
  if (bounded) {
    value.assign(clipboard, length);
  }
  sodium_memzero(clipboard, bounded ? length : kMaximumComponentSize + 1);
  SDL_free(clipboard);
  if (!bounded || !valid_clipboard_field(field, value)) {
    mp_secure_clear_string(value);
    return false;
  }
  if (field == kAddressField) {
    replace_field(data.direct_address, value);
  } else if (field == kPortField) {
    replace_field(data.direct_port, value);
  } else {
    replace_field(data.direct_token, value);
  }
  mp_secure_clear_string(value);
  return true;
}

template <size_t Size>
bool edit_buffer(std::array<char, Size>& buffer, char character, bool backspace) {
  const size_t length = strnlen(buffer.data(), buffer.size());
  if (backspace) {
    if (length == 0) {
      return false;
    }
    buffer[length - 1] = '\0';
    return true;
  }
  if (character == '\0' || length + 1 >= buffer.size()) {
    return false;
  }
  buffer[length] = character;
  buffer[length + 1] = '\0';
  return true;
}
}  // namespace

void mp_direct_connect_clear(MultiplayerData& data) {
  multiplayer_clear_direct_connect_draft(data);
}

void mp_direct_connect_reset(MultiplayerData& data) {
  mp_direct_connect_clear(data);
  replace_field(data.direct_port, std::string(kDefaultPort));
}

std::string mp_direct_connect_display(const MultiplayerData& data, int field) {
  if (field == kAddressField) {
    return data.direct_address.data();
  }
  if (field == kPortField) {
    return data.direct_port.data();
  }
  if (field == kTokenField) {
    return std::string(strnlen(data.direct_token.data(), data.direct_token.size()), '*');
  }
  return {};
}

int mp_direct_connect_edit(MultiplayerData& data, int field, uint32_t key) {
  const SDL_Keymod modifiers = SDL_GetModState();
  if ((modifiers & SDL_KMOD_CTRL) != 0 && (key == 'v' || key == 'V')) {
    return paste_field(data, field) ? 1 : -1;
  }
  const bool backspace = key == SDLK_BACKSPACE;
  char character = key <= 0x7f ? static_cast<char>(key) : '\0';
  if ((modifiers & SDL_KMOD_SHIFT) != 0 && character >= 'a' && character <= 'z') {
    character = static_cast<char>(character - 'a' + 'A');
  } else if ((modifiers & SDL_KMOD_SHIFT) != 0 && character == '-' && field == kTokenField) {
    character = '_';
  }
  if (!backspace) {
    const bool address_character =
        field == kAddressField &&
        ((character >= '0' && character <= '9') || character == '.');
    const bool port_character = field == kPortField && character >= '0' && character <= '9';
    const bool token_character =
        field == kTokenField &&
        ((character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z') ||
         (character >= '0' && character <= '9') || character == '-' || character == '_');
    if (!address_character && !port_character && !token_character) {
      return 0;
    }
  }
  if (field == kAddressField) {
    return edit_buffer(data.direct_address, character, backspace) ? 1 : 0;
  }
  if (field == kPortField) {
    return edit_buffer(data.direct_port, character, backspace) ? 1 : 0;
  }
  if (field == kTokenField) {
    return edit_buffer(data.direct_token, character, backspace) ? 1 : 0;
  }
  return 0;
}

bool mp_direct_connect_ready(const MultiplayerData& data) {
  return valid_parts(data.direct_address.data(), port_value(data), data.direct_token.data());
}

bool mp_direct_connect_start(MultiplayerData& data) {
  if (!mp_direct_connect_ready(data)) {
    return false;
  }
  const std::string address(data.direct_address.data());
  const uint16_t port = port_value(data);
  std::string token(data.direct_token.data());
  if (token.empty()) {
    return MultiplayerScanner::start_direct_search(data, address, port);
  }

  std::string invite = address + ":" + std::to_string(port) + "/" + token;
  mp_secure_clear_string(token);
  mp_direct_connect_clear(data);
  MultiplayerManager::disconnect(data);
  std::string parsed_address;
  uint16_t parsed_port = 0;
  const bool valid = data.security.start_client(invite, parsed_address, parsed_port);
  mp_secure_clear_string(invite);
  if (!valid) {
    data.join_status = static_cast<int>(MultiplayerStatus::FAILED);
    return false;
  }
  MultiplayerManager::setup_client(data, parsed_address.c_str(), parsed_port);
  return data.join_status != static_cast<int>(MultiplayerStatus::FAILED);
}
