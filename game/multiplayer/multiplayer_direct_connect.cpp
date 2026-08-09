#include "game/multiplayer/multiplayer_direct_connect.h"

#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>

#include <sodium.h>

#include "game/multiplayer/multiplayer_manager.h"
#include "game/multiplayer/multiplayer_preferences.h"
#include "game/multiplayer/multiplayer_scanner.h"
#include "game/multiplayer/multiplayer_security.h"
#include "game/multiplayer/multiplayer_session.h"

#include "third-party/SDL/include/SDL3/SDL.h"

namespace {
constexpr int kAddressField = 0;
constexpr int kPortField = 1;
constexpr int kRoomCodeField = 2;
constexpr const char* kValidationRoomCode = "ABC123";

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

bool valid_parts(const std::string& address, uint16_t port, const std::string& room_code) {
  std::string normalized;
  if (address.empty() || !mp_valid_gameplay_port(port) ||
      !mp_normalize_room_code(room_code, normalized)) {
    return false;
  }
  MultiplayerSecurity validator;
  std::string parsed_address;
  uint16_t parsed_port = 0;
  std::string invite = "jad2mp://" + address + ":" + std::to_string(port) + "/" +
                       (normalized.empty() ? kValidationRoomCode : normalized);
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
    return result.ec == std::errc() && result.ptr == value.data() + value.size() &&
           mp_valid_gameplay_port(port);
  }
  std::string normalized;
  return field == kRoomCodeField && mp_normalize_room_code(value, normalized, false);
}

bool paste_field(MultiplayerData& data, int field) {
  char* clipboard = SDL_GetClipboardText();
  if (!clipboard) {
    return false;
  }
  constexpr size_t kMaximumComponentSize = 15;
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
    std::string normalized;
    if (!mp_normalize_room_code(value, normalized, false)) {
      return false;
    }
    replace_field(data.direct_room_code, normalized);
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
  replace_field(data.direct_port, std::to_string(mp_multiplayer_preferences().network_port));
}

std::string mp_direct_connect_display(const MultiplayerData& data, int field) {
  if (field == kAddressField) {
    return data.direct_address.data();
  }
  if (field == kPortField) {
    return data.direct_port.data();
  }
  if (field == kRoomCodeField) {
    return data.direct_room_code.data();
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
  } else if (character >= 'a' && character <= 'z' && field == kRoomCodeField) {
    character = static_cast<char>(character - 'a' + 'A');
  }
  if (!backspace) {
    const bool address_character =
        field == kAddressField &&
        ((character >= '0' && character <= '9') || character == '.');
    const bool port_character = field == kPortField && character >= '0' && character <= '9';
    const bool room_code_character =
        field == kRoomCodeField &&
        ((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'));
    if (!address_character && !port_character && !room_code_character) {
      return 0;
    }
  }
  if (field == kAddressField) {
    return edit_buffer(data.direct_address, character, backspace) ? 1 : 0;
  }
  if (field == kPortField) {
    return edit_buffer(data.direct_port, character, backspace) ? 1 : 0;
  }
  if (field == kRoomCodeField) {
    return edit_buffer(data.direct_room_code, character, backspace) ? 1 : 0;
  }
  return 0;
}

bool mp_direct_connect_ready(const MultiplayerData& data) {
  return valid_parts(data.direct_address.data(), port_value(data), data.direct_room_code.data());
}

bool mp_direct_connect_start(MultiplayerData& data) {
  if (!mp_direct_connect_ready(data)) {
    return false;
  }
  const std::string address(data.direct_address.data());
  const uint16_t port = port_value(data);
  std::string room_code(data.direct_room_code.data());
  if (room_code.empty()) {
    return MultiplayerScanner::start_direct_search(data, address, port);
  }

  std::string invite = "jad2mp://" + address + ":" + std::to_string(port) + "/" + room_code;
  mp_secure_clear_string(room_code);
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
