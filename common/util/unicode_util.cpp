#include "unicode_util.h"

#include <algorithm>

// clang-format off
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <Stringapiset.h>
#include <processenv.h>
#include <winbase.h>
#include <shellapi.h>
#undef FALSE
#endif
// clang-format on

#ifdef _WIN32
std::wstring utf8_string_to_wide_string(const std::string_view& str) {
  std::wstring ret;
  if (!utf8_string_to_wide_string(ret, str))
    return {};

  return ret;
}

bool utf8_string_to_wide_string(std::wstring& dest, const std::string_view& str) {
  int wlen =
      MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), nullptr, 0);
  if (wlen < 0)
    return false;

  dest.resize(wlen);
  if (wlen > 0 && MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()),
                                      dest.data(), wlen) < 0)
    return false;

  return true;
}

std::string wide_string_to_utf8_string(const std::wstring_view& str) {
  std::string ret;
  if (!wide_string_to_utf8_string(ret, str))
    ret.clear();

  return ret;
}

bool wide_string_to_utf8_string(std::string& dest, const std::wstring_view& str) {
  int mblen = WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), nullptr,
                                  0, nullptr, nullptr);
  if (mblen < 0)
    return false;

  dest.resize(mblen);
  if (mblen > 0 && WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.length()),
                                       dest.data(), mblen, nullptr, nullptr) < 0) {
    return false;
  }

  return true;
}

#endif

namespace unicode {
namespace {

bool is_continuation(const unsigned char byte) {
  return (byte & 0xc0) == 0x80;
}

}  // namespace

bool decode_utf8_codepoint(const std::string_view text, size_t& offset, char32_t& codepoint) {
  if (offset >= text.size()) {
    return false;
  }
  const auto first = static_cast<unsigned char>(text[offset]);
  size_t length = 0;
  char32_t value = 0;
  char32_t minimum = 0;
  if (first < 0x80) {
    length = 1;
    value = first;
  } else if ((first & 0xe0) == 0xc0) {
    length = 2;
    value = first & 0x1f;
    minimum = 0x80;
  } else if ((first & 0xf0) == 0xe0) {
    length = 3;
    value = first & 0x0f;
    minimum = 0x800;
  } else if ((first & 0xf8) == 0xf0) {
    length = 4;
    value = first & 0x07;
    minimum = 0x10000;
  } else {
    return false;
  }
  if (offset + length > text.size()) {
    return false;
  }
  for (size_t index = 1; index < length; ++index) {
    const auto byte = static_cast<unsigned char>(text[offset + index]);
    if (!is_continuation(byte)) {
      return false;
    }
    value = (value << 6) | (byte & 0x3f);
  }
  if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
    return false;
  }
  offset += length;
  codepoint = value;
  return true;
}

bool decode_utf8(const std::string_view text, std::vector<char32_t>& codepoints) {
  codepoints.clear();
  size_t offset = 0;
  while (offset < text.size()) {
    char32_t codepoint = 0;
    if (!decode_utf8_codepoint(text, offset, codepoint)) {
      codepoints.clear();
      return false;
    }
    codepoints.push_back(codepoint);
  }
  return true;
}

bool encode_utf8_codepoint(const char32_t codepoint, std::string& output) {
  if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
    return false;
  }
  if (codepoint <= 0x7f) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
  return true;
}

std::string encode_utf8_codepoint(const char32_t codepoint) {
  std::string output;
  encode_utf8_codepoint(codepoint, output);
  return output;
}

bool is_valid_utf8(const std::string_view text) {
  size_t offset = 0;
  while (offset < text.size()) {
    if (char32_t codepoint = 0; !decode_utf8_codepoint(text, offset, codepoint)) {
      return false;
    }
  }
  return true;
}

size_t previous_utf8_boundary(const std::string_view text, size_t offset) {
  offset = std::min(offset, text.size());
  if (offset == 0) {
    return 0;
  }
  --offset;
  while (offset > 0 && is_continuation(static_cast<unsigned char>(text[offset]))) {
    --offset;
  }
  return offset;
}

size_t next_utf8_boundary(const std::string_view text, size_t offset) {
  if (offset >= text.size()) {
    return text.size();
  }
  if (is_continuation(static_cast<unsigned char>(text[offset]))) {
    do {
      ++offset;
    } while (offset < text.size() &&
             is_continuation(static_cast<unsigned char>(text[offset])));
    return offset;
  }
  if (char32_t codepoint = 0; !decode_utf8_codepoint(text, offset, codepoint)) {
    return std::min(text.size(), offset + 1);
  }
  return offset;
}

size_t utf8_codepoint_to_byte_offset(const std::string_view text, const size_t codepoint_offset) {
  size_t offset = 0;
  for (size_t index = 0; index < codepoint_offset && offset < text.size(); ++index) {
    if (char32_t codepoint = 0; !decode_utf8_codepoint(text, offset, codepoint)) {
      return text.size();
    }
  }
  return offset;
}

}  // namespace unicode

std::string get_env(const std::string& name) {
#ifdef _WIN32
  auto name_wide = utf8_string_to_wide_string(name);
  auto val = _wgetenv(name_wide.data());
  if (!val) {
    return "";
  }
  return wide_string_to_utf8_string(val);
#else
  auto val = std::getenv(name.data());
  if (!val) {
    return "";
  }
  return val;
#endif
}
