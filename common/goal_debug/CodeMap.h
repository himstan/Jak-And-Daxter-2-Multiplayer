#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/link_types.h"

namespace goal_debug {

struct CodeMapLine {
  u32 offset = 0;
  u32 file = 0;
  int line = 0;
};

struct CodeMapFunction {
  u8 seg = 0;
  u32 offset = 0;
  u32 length = 0;
  std::optional<int> stack_usage;
  std::string name;
  std::vector<CodeMapLine> lines;

  const CodeMapLine* line_at(u32 offset_in_function) const;
};

struct CodeMap {
  u32 segment_sizes[N_SEG] = {0, 0, 0};
  std::vector<std::string> files;
  std::vector<CodeMapFunction> functions;

  const CodeMapFunction* function_at(u8 seg, u32 offset_in_seg) const;
  
  bool matches_loaded_sizes(const u32 loaded_sizes[N_SEG]) const;

  std::string to_text() const;
  static std::optional<CodeMap> from_text(const std::string& text);
};

}  // namespace goal_debug
