#include "game/kernel/jak2/kcrash.h"

#include <string>

#include "common/goal_constants.h"
#include "common/symbols.h"

#include "game/kernel/common/kcrash.h"
#include "game/kernel/common/kscheme.h"

#include "fmt/format.h"

namespace jak2 {
namespace {
constexpr u32 kTypeSymbol = 0;
constexpr u32 kTypeParent = 4;
constexpr u32 kStringData = 4;
constexpr u32 kProcessTreeName = 0;
constexpr u32 kProcessState = 60;
constexpr u32 kStackFrameName = 0;
constexpr u32 kThreadProcess = 4;

constexpr int kMaxTypeDepth = 32;
constexpr u32 kSymbolTableHalfSize = jak2::GOAL_MAX_SYMBOLS * 2;

u32 read_u32(u32 addr) {
  u32 value = 0;
  kcrash::read_memory(addr, &value, sizeof(value));
  return value;
}

bool is_symbol(u32 addr) {
  u32 from_table_start = addr - (s7.offset - kSymbolTableHalfSize);
  return (from_table_start & 3) == 0 && from_table_start < 2 * kSymbolTableHalfSize;
}

std::string symbol_name(u32 symbol) {
  if (!is_symbol(symbol)) {
    return {};
  }
  u32 name = read_u32(symbol + jak2::SYM_TO_STRING_OFFSET);
  return name ? kcrash::read_c_string(name + kStringData) : std::string();
}

std::string type_name(u32 type) {
  u32 symbol = read_u32(type + kTypeSymbol);
  if (type == 0 || !is_symbol(symbol) || read_u32(symbol - 1) != type) {
    return {};
  }
  return symbol_name(symbol);
}

u32 type_of_basic(u32 object) {
  return (object & 7) == BASIC_OFFSET ? read_u32(object - BASIC_OFFSET) : 0;
}

bool type_is_a(u32 type, const char* ancestor) {
  for (int i = 0; i < kMaxTypeDepth; i++) {
    auto name = type_name(type);
    if (name.empty()) {
      return false;
    }
    if (name == ancestor) {
      return true;
    }
    u32 parent = read_u32(type + kTypeParent);
    if (parent == type) {
      return false;
    }
    type = parent;
  }
  return false;
}

std::string process_name(u32 process) {
  return kcrash::read_c_string(read_u32(process + kProcessTreeName) + kStringData);
}

std::string describe_value(u32 value) {
  if (value == s7.offset) {
    return "#f";
  }
  if (value == s7.offset + jak2_symbols::FIX_SYM_TRUE) {
    return "#t";
  }
  auto name = symbol_name(value);
  if (!name.empty()) {
    return "'" + name;
  }
  name = type_name(value);
  if (!name.empty()) {
    return "type " + name;
  }
  u32 type = type_of_basic(value);
  name = type_name(type);
  if (name.empty()) {
    return {};
  }
  if (name == "string") {
    return fmt::format("\"{}\"", kcrash::read_c_string(value + kStringData));
  }
  if (type_is_a(type, "process")) {
    return fmt::format("#<{} \"{}\">", name, process_name(value));
  }
  return fmt::format("#<{}>", name);
}

std::string describe_process(u32 process) {
  if (type_is_a(type_of_basic(process), "thread")) {
    process = read_u32(process + kThreadProcess);
  }
  u32 type = type_of_basic(process);
  if (!type_is_a(type, "process")) {
    return {};
  }
  u32 state = read_u32(process + kProcessState);
  std::string state_name;
  if (type_is_a(type_of_basic(state), "state")) {
    state_name = symbol_name(read_u32(state + kStackFrameName));
  }
  return fmt::format("{} (type {}), state {}", process_name(process), type_name(type),
                     state_name.empty() ? "none" : state_name);
}

}  // namespace

void install_crash_describers() {
  kcrash::set_game_describers({describe_value, describe_process});
}

}  // namespace jak2
