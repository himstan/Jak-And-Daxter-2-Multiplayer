#include "CodeMap.h"

#include <cstdlib>
#include <sstream>

#include "fmt/format.h"

namespace goal_debug {

const CodeMapLine* CodeMapFunction::line_at(u32 offset_in_function) const {
  const CodeMapLine* result = nullptr;
  for (const auto& line : lines) {
    if (line.offset > offset_in_function) {
      break;
    }
    result = &line;
  }
  return result;
}

const CodeMapFunction* CodeMap::function_at(u8 seg, u32 offset_in_seg) const {
  for (const auto& function : functions) {
    if (function.seg == seg && offset_in_seg >= function.offset &&
        offset_in_seg < function.offset + function.length) {
      return &function;
    }
  }
  return nullptr;
}

bool CodeMap::matches_loaded_sizes(const u32 loaded_sizes[N_SEG]) const {
  for (int seg = 0; seg < N_SEG; seg++) {
    if (loaded_sizes[seg] != 0 && loaded_sizes[seg] != segment_sizes[seg]) {
      return false;
    }
  }
  return true;
}
  
std::string CodeMap::to_text() const {
  std::string result =
      fmt::format("sizes {} {} {}\n", segment_sizes[0], segment_sizes[1], segment_sizes[2]);
  int current_file = -1;
  for (const auto& function : functions) {
    result += fmt::format("fn {} {} {} {} {}\n", function.seg, function.offset, function.length,
                          function.stack_usage ? std::to_string(*function.stack_usage) : "-",
                          function.name);
    for (const auto& line : function.lines) {
      if (int(line.file) != current_file) {
        current_file = line.file;
        result += fmt::format("file {}\n", files.at(line.file));
      }
      result += fmt::format("ln {} {}\n", line.offset, line.line);
    }
  }
  return result;
}

std::optional<CodeMap> CodeMap::from_text(const std::string& text) {
  CodeMap map;
  bool got_sizes = false;
  std::istringstream stream(text);
  std::string record;
  while (std::getline(stream, record)) {
    if (!record.empty() && record.back() == '\r') {
      record.pop_back();
    }
    std::istringstream fields(record);
    std::string kind;
    fields >> kind;
    if (kind.empty()) {
      continue;
    }
    if (kind == "sizes") {
      fields >> map.segment_sizes[0] >> map.segment_sizes[1] >> map.segment_sizes[2];
      got_sizes = true;
    } else if (kind == "fn") {
      CodeMapFunction function;
      int seg = 0;
      std::string stack;
      fields >> seg >> function.offset >> function.length >> stack;
      function.seg = seg;
      if (stack != "-") {
        function.stack_usage = std::atoi(stack.c_str());
      }
      std::getline(fields >> std::ws, function.name);
      map.functions.push_back(function);
    } else if (kind == "file") {
      std::string path;
      std::getline(fields >> std::ws, path);
      map.files.push_back(path);
    } else if (kind == "ln") {
      if (map.functions.empty() || map.files.empty()) {
        return std::nullopt;
      }
      CodeMapLine line;
      fields >> line.offset >> line.line;
      line.file = map.files.size() - 1;
      map.functions.back().lines.push_back(line);
    } else {
      return std::nullopt;
    }
    if (fields.fail()) {
      return std::nullopt;
    }
  }
  if (!got_sizes) {
    return std::nullopt;
  }
  return map;
}

}  // namespace goal_debug
