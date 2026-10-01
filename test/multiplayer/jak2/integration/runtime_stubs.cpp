#include "common/common_types.h"
#include "common/versions/versions.h"

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak2/kscheme.h"

u8* g_ee_main_mem = nullptr;
GameVersion g_game_version = GameVersion::Jak2;
int g_argc = 0;
const char** g_argv = nullptr;
Ptr<u32> s7;

namespace jak2 {
u64 make_string_from_c(const char*) {
  return 0;
}

Ptr<Function> make_function_symbol_from_c(const char*, void*) {
  return Ptr<Function>(0);
}
}  // namespace jak2
