#include "DebugInfo.h"

#include <algorithm>
#include <utility>

#include "common/goos/Reader.h"

#include "fmt/format.h"

DebugInfo::DebugInfo(std::string obj_name) : m_obj_name(std::move(obj_name)) {}

std::string FunctionDebugInfo::disassemble_debug_info(bool* had_failure,
                                                      const goos::Reader* reader,
                                                      bool omit_ir) {
  std::string result = fmt::format("[{}]\n", name);
  result += disassemble_x86_function(generated_code.data(), generated_code.size(), reader, 0x10000,
                                     0x10000, instructions, code_sources, ir_strings, had_failure,
                                     true, omit_ir);

  return result;
}

std::string DebugInfo::disassemble_all_functions(bool* had_failure,
                                                 const goos::Reader* reader,
                                                 bool omit_ir) {
  std::string result;
  for (auto& kv : m_functions) {
    result += kv.second.disassemble_debug_info(had_failure, reader, omit_ir) + "\n\n";
  }
  return result;
}

std::string DebugInfo::disassemble_function_by_name(const std::string& name,
                                                    bool* had_failure,
                                                    const goos::Reader* reader) {
  std::string result;
  for (auto& kv : m_functions) {
    if (kv.second.name == name) {
      result += kv.second.disassemble_debug_info(had_failure, reader, false) + "\n\n";
    }
  }
  return result;
}

goal_debug::CodeMap DebugInfo::code_map(const goos::Reader& reader) const {
  goal_debug::CodeMap map;
  std::unordered_map<std::string, u32> file_indices;

  for (const auto& [name, info] : m_functions) {
    goal_debug::CodeMapFunction function;
    function.seg = info.seg;
    function.offset = info.offset_in_seg;
    function.length = info.length;
    function.stack_usage = info.stack_usage;
    function.name = name;

    const goos::HeapObject* last_source = nullptr;
    for (const auto& instruction : info.instructions) {
      if (instruction.kind != InstructionInfo::Kind::IR || instruction.ir_idx < 0 ||
          instruction.ir_idx >= int(info.code_sources.size())) {
        continue;
      }
      const auto& source = info.code_sources[instruction.ir_idx];
      if (source.get() == last_source) {
        continue;
      }
      last_source = source.get();
      auto location = reader.db.try_get_short_info(source, false);
      if (!location) {
        continue;
      }
      auto file = file_indices.try_emplace(location->filename, u32(map.files.size())).first;
      if (file->second == map.files.size()) {
        map.files.push_back(location->filename);
      }
      if (function.lines.empty() || function.lines.back().file != file->second ||
          function.lines.back().line != location->line_idx_to_display) {
        function.lines.push_back(
            {u32(instruction.offset), file->second, location->line_idx_to_display});
      }
    }
    map.functions.push_back(std::move(function));
  }

  std::sort(map.functions.begin(), map.functions.end(), [](const auto& a, const auto& b) {
    return a.seg != b.seg ? a.seg < b.seg : a.offset < b.offset;
  });
  return map;
}
