#pragma once

#include <string>

#include "common/common_types.h"

struct SegmentInfo;

namespace kcrash {
struct GameDescribers {
  std::string (*value)(u32 goal_value) = nullptr;
  std::string (*process)(u32 process_register) = nullptr;
};

void install();

void set_game_describers(const GameDescribers& describers);

void note_object_loaded(const char* name, const SegmentInfo* code_infos);

bool read_memory(u32 goal_addr, void* out, u32 size);

std::string read_c_string(u32 goal_addr);

}  // namespace kcrash
