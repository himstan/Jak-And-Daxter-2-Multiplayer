#include "kcrash.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <semaphore>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/cross_os_debug/xdbg.h"
#include "common/goal_constants.h"
#include "common/goal_debug/CodeMap.h"
#include "common/link_types.h"
#include "common/util/FileUtil.h"
#include "common/util/string_util.h"
#include "common/versions/versions.h"

#include "game/kernel/common/klink.h"
#include "game/runtime.h"

#include "fmt/format.h"

#if defined(__linux__) && defined(__x86_64__)
#include <csignal>
#include <pthread.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace kcrash {
namespace {

constexpr int kMaxFrames = 48;
constexpr int kMaxScannedStackWords = 256;
constexpr int kMaxStringLength = 96;
constexpr auto kReportTimeout = std::chrono::seconds(3);

// indices into xdbg::Regs::gprs
constexpr int kRsp = 4;
constexpr int kProcessRegister = 13;
constexpr const char* kRegisterNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                            "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

struct LoadedSegment {
  std::string object_name;
  u8 seg = 0;
  u32 start = 0;
  u32 size = 0;
  u32 object_sizes[N_SEG] = {0, 0, 0};
};

struct Fault {
  xdbg::Regs regs;
  const char* kind = "";
  bool has_address = false;
  const char* access = "accessing";
  u64 address = 0;
};

struct State {
  std::binary_semaphore wake{0};
  std::binary_semaphore done{0};
  std::atomic<bool> claimed{false};
  Fault fault;
  std::string report_path;
  fs::path map_dir;
};
  
std::vector<LoadedSegment> g_segments;
GameDescribers g_describers;
State* g_state = nullptr;

bool is_goal_memory(u64 host_address) {
  return host_address >= u64(g_ee_main_mem) && host_address < u64(g_ee_main_mem) + EE_MAIN_MEM_SIZE;
}

const LoadedSegment* find_segment(u32 goal_addr) {
  for (const auto& segment : g_segments) {
    if (goal_addr >= segment.start && goal_addr - segment.start < segment.size) {
      return &segment;
    }
  }
  return nullptr;
}
  
class Symbolizer {
 public:
  struct Location {
    const LoadedSegment* segment = nullptr;
    const goal_debug::CodeMap* map = nullptr;
    const goal_debug::CodeMapFunction* function = nullptr;
    u32 offset_in_function = 0;
  };

  Location locate(u32 goal_addr) {
    Location result;
    result.segment = find_segment(goal_addr);
    if (!result.segment) {
      return result;
    }
    result.map = map_for(*result.segment);
    if (result.map) {
      u32 offset_in_seg = goal_addr - result.segment->start;
      result.function = result.map->function_at(result.segment->seg, offset_in_seg);
      if (result.function) {
        result.offset_in_function = offset_in_seg - result.function->offset;
      }
    }
    return result;
  }
  
  std::string describe(u32 goal_addr, bool is_return_address) {
    auto location = locate(goal_addr);
    if (!location.segment) {
      return fmt::format("GOAL address 0x{:x}, not in a loaded object file\n", goal_addr);
    }
    const auto& segment = *location.segment;
    std::string where = fmt::format("{}, {} segment +0x{:x}", segment.object_name,
                                    SEGMENT_NAMES[segment.seg], goal_addr - segment.start);
    if (!location.function) {
      return fmt::format("{} ({})\n", where,
                         location.map ? "not inside a function" : "no matching code map");
    }
    std::string result = fmt::format("{}  [{}]\n", location.function->name, where);
    u32 offset = location.offset_in_function - (is_return_address ? 1 : 0);
    if (const auto* line = location.function->line_at(offset)) {
      const auto& file = location.map->files.at(line->file);
      result += fmt::format("      {}:{}\n", fs::path(file).filename().string(), line->line);
      auto text = source_line(file, line->line);
      if (!text.empty()) {
        result += fmt::format("      -> {}\n", text);
      }
    }
    return result;
  }

 private:
  const goal_debug::CodeMap* map_for(const LoadedSegment& segment) {
    auto [entry, is_new] = m_maps.try_emplace(segment.object_name);
    if (is_new) {
      std::ifstream file(g_state->map_dir / (segment.object_name + ".map"), std::ios::binary);
      if (file) {
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        entry->second = goal_debug::CodeMap::from_text(text);
      }
    }
    if (entry->second && entry->second->matches_loaded_sizes(segment.object_sizes)) {
      return &*entry->second;
    }
    return nullptr;
  }

  static std::string source_line(const std::string& path, int line_number) {
    std::ifstream file(path);
    if (!file) {
      file.open(file_util::get_jak_project_dir() / path);
    }
    std::string text;
    for (int i = 0; i < line_number && std::getline(file, text); i++) {
    }
    if (!file) {
      return {};
    }
    auto start = text.find_first_not_of(" \t");
    auto end = text.find_last_not_of(" \t\r");
    return start == std::string::npos ? std::string() : text.substr(start, end - start + 1);
  }

  std::unordered_map<std::string, std::optional<goal_debug::CodeMap>> m_maps;
};

std::string describe_fault(const Fault& fault) {
  std::string result = fmt::format("GOAL crash: {}", fault.kind);
  if (!fault.has_address) {
    return result + "\n";
  }
  u64 base = u64(g_ee_main_mem);
  if (fault.address >= base && fault.address - base <= UINT32_MAX) {
    u64 goal_addr = fault.address - base;
    result += fmt::format(" {} GOAL address 0x{:x}", fault.access, goal_addr);
    if (goal_addr < u64(EE_MAIN_MEM_LOW_PROTECT)) {
      result += fmt::format(" (null pointer + {})", goal_addr);
    } else if (goal_addr >= u64(EE_MAIN_MEM_SIZE)) {
      result += " (beyond the end of GOAL memory)";
    }
  } else {
    result +=
        fmt::format(" {} host address 0x{:x} (not a GOAL address)", fault.access, fault.address);
  }
  return result + "\n";
}

std::string describe_registers(const xdbg::Regs& regs) {
  std::string result;
  for (int i = 0; i < 16; i++) {
    u64 value = regs.gprs[i];
    std::string note;
    if (value <= UINT32_MAX) {
      if (g_describers.value) {
        note = g_describers.value(u32(value));
      }
    } else if (is_goal_memory(value)) {
      note = fmt::format("host pointer to GOAL address 0x{:x}", value - u64(g_ee_main_mem));
    }
    result += fmt::format("  {:<3} 0x{:016x}  {}\n", kRegisterNames[i], value, note);
  }
  return result;
}

std::string describe_code_bytes(u64 rip) {
  constexpr u32 kBytesEachSide = 16;
  u8 bytes[kBytesEachSide * 2];
  if (!is_goal_memory(rip) ||
      !read_memory(u32(rip - u64(g_ee_main_mem)) - kBytesEachSide, bytes, sizeof(bytes))) {
    return {};
  }
  std::string result = "\nCode bytes around the fault (| marks the faulting instruction)\n ";
  for (u32 i = 0; i < sizeof(bytes); i++) {
    result += fmt::format("{}{:02x}", i == kBytesEachSide ? " | " : " ", bytes[i]);
  }
  return result + "\n";
}
  
bool scan_for_return_address(u32* sp, u32* pc) {
  for (int i = 0; i < kMaxScannedStackWords; i++) {
    u64 word = 0;
    if (!read_memory(*sp, &word, sizeof(word))) {
      return false;
    }
    *sp += sizeof(word);
    if (is_goal_memory(word) && find_segment(u32(word - u64(g_ee_main_mem)))) {
      *pc = u32(word - u64(g_ee_main_mem));
      return true;
    }
  }
  return false;
}
  
std::string describe_backtrace(const xdbg::Regs& regs, Symbolizer& symbolizer) {
  std::string result = "\nBacktrace (innermost first, ? = found by scanning the stack)\n";
  u64 base = u64(g_ee_main_mem);
  if (!is_goal_memory(regs.gprs[kRsp])) {
    return result +
           "  The stack is not in GOAL memory, so this fault was not in GOAL code or in "
           "native code called from it.\n";
  }
  u32 sp = u32(regs.gprs[kRsp] - base);
  u32 pc = 0;
  bool scanned = false;
  bool is_return_address = false;

  if (is_goal_memory(regs.rip) && find_segment(u32(regs.rip - base))) {
    pc = u32(regs.rip - base);
  } else {
    result += regs.rip == base
                  ? "    call through a null function pointer\n"
                  : fmt::format("    0x{:016x}, not in a loaded object file\n", regs.rip);
    scanned = true;
    is_return_address = true;
    if (!scan_for_return_address(&sp, &pc)) {
      return result + "  No return address into GOAL code was found on the stack.\n";
    }
  }

  for (int frame = 0; frame < kMaxFrames; frame++) {
    result +=
        fmt::format("  {} {}", scanned ? '?' : ' ', symbolizer.describe(pc, is_return_address));
    auto location = symbolizer.locate(pc);
    u64 return_address = 0;
    is_return_address = true;
    if (location.function && location.function->stack_usage &&
        read_memory(sp + *location.function->stack_usage, &return_address,
                    sizeof(return_address)) &&
        is_goal_memory(return_address) && find_segment(u32(return_address - base))) {
      sp += *location.function->stack_usage + sizeof(return_address);
      pc = u32(return_address - base);
      scanned = false;
    } else if (scan_for_return_address(&sp, &pc)) {
      scanned = true;
    } else {
      break;
    }
  }
  return result;
}

std::string build_report(const Fault& fault) {
  Symbolizer symbolizer;
  u64 base = u64(g_ee_main_mem);
  std::string report = describe_fault(fault);

  report += "\nAt ";
  if (is_goal_memory(fault.regs.rip)) {
    report += symbolizer.describe(u32(fault.regs.rip - base), false);
  } else {
    report +=
        fmt::format("host address 0x{:016x}, outside GOAL memory (native code)\n", fault.regs.rip);
  }

  std::string process;
  if (g_describers.process && fault.regs.gprs[kProcessRegister] <= UINT32_MAX) {
    process = g_describers.process(u32(fault.regs.gprs[kProcessRegister]));
  }
  report += fmt::format("\nProcess: {}\n", process.empty() ? "none running" : process);

  report += describe_backtrace(fault.regs, symbolizer);
  report += "\nRegisters\n" + describe_registers(fault.regs);
  report += fmt::format("  rip 0x{:016x}\n", fault.regs.rip);
  report += describe_code_bytes(fault.regs.rip);
  report += fmt::format("\nGOAL memory base 0x{:016x}\n", base);
  return report;
}

void reporter_thread() {
  g_state->wake.acquire();
  std::string report = build_report(g_state->fault);
  if (FILE* file = fopen(g_state->report_path.c_str(), "w")) {
    fputs(report.c_str(), file);
    fclose(file);
  }
  fprintf(stderr, "\n%s\nCrash report written to %s\n", report.c_str(),
          g_state->report_path.c_str());
  fflush(stderr);
  g_state->done.release();
}
  
void report_fault_and_wait() {
  if (g_state->fault.has_address && g_state->fault.address == g_state->fault.regs.rip) {
    g_state->fault.access = "executing";
  }
  g_state->wake.release();
  (void)g_state->done.try_acquire_for(kReportTimeout);
}

#ifdef _WIN32

DWORD g_goal_thread_id = 0;

bool is_fatal_exception(DWORD code, u64 rip) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
      return true;
    case EXCEPTION_BREAKPOINT:
      return is_goal_memory(rip);
    default:
      return false;
  }
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
  const auto& record = *info->ExceptionRecord;
  const auto& context = *info->ContextRecord;
  if (GetCurrentThreadId() != g_goal_thread_id ||
      !is_fatal_exception(record.ExceptionCode, context.Rip) || g_state->claimed.exchange(true)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  auto& fault = g_state->fault;
  const u64 gprs[16] = {context.Rax, context.Rcx, context.Rdx, context.Rbx,
                        context.Rsp, context.Rbp, context.Rsi, context.Rdi,
                        context.R8,  context.R9,  context.R10, context.R11,
                        context.R12, context.R13, context.R14, context.R15};
  memcpy(fault.regs.gprs, gprs, sizeof(gprs));
  fault.regs.rip = context.Rip;
  fault.kind = xdbg::win32_exception_code_to_charp(record.ExceptionCode);
  if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
       record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
      record.NumberParameters >= 2) {
    fault.has_address = true;
    fault.address = record.ExceptionInformation[1];
    fault.access = record.ExceptionInformation[0] == 1 ? "writing" : "reading";
  }
  report_fault_and_wait();
  return EXCEPTION_CONTINUE_SEARCH;
}

void install_platform_handler() {
  g_goal_thread_id = GetCurrentThreadId();
  AddVectoredExceptionHandler(1, on_exception);
}

u64 current_process_id() {
  return GetCurrentProcessId();
}

#elif defined(__linux__) && defined(__x86_64__)

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP};
constexpr int kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);
struct sigaction g_previous_actions[kSignalCount];
pthread_t g_goal_thread;
u8 g_signal_stack[1 << 16];

const char* signal_name(int signal) {
  switch (signal) {
    case SIGSEGV:
      return "SIGSEGV (invalid memory access)";
    case SIGBUS:
      return "SIGBUS (invalid memory access)";
    case SIGFPE:
      return "SIGFPE (divide by zero or arithmetic error)";
    case SIGILL:
      return "SIGILL (illegal instruction)";
    default:
      return "SIGTRAP (breakpoint)";
  }
}

void on_signal(int signal, siginfo_t* info, void* raw_context) {
  if (pthread_equal(pthread_self(), g_goal_thread) && !g_state->claimed.exchange(true)) {
    const auto& registers = static_cast<ucontext_t*>(raw_context)->uc_mcontext.gregs;
    auto& fault = g_state->fault;
    const int order[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                           REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
    for (int i = 0; i < 16; i++) {
      fault.regs.gprs[i] = registers[order[i]];
    }
    fault.regs.rip = registers[REG_RIP];
    fault.kind = signal_name(signal);
    if (signal == SIGSEGV || signal == SIGBUS) {
      fault.has_address = true;
      fault.address = u64(info->si_addr);
      fault.access = (registers[REG_ERR] & 2) ? "writing" : "reading";
    }
    report_fault_and_wait();
  }
  for (int i = 0; i < kSignalCount; i++) {
    sigaction(kSignals[i], &g_previous_actions[i], nullptr);
  }
  raise(signal);
}

void install_platform_handler() {
  g_goal_thread = pthread_self();
  stack_t signal_stack = {};
  signal_stack.ss_sp = g_signal_stack;
  signal_stack.ss_size = sizeof(g_signal_stack);
  sigaltstack(&signal_stack, nullptr);

  struct sigaction action = {};
  action.sa_sigaction = on_signal;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&action.sa_mask);
  for (int i = 0; i < kSignalCount; i++) {
    sigaction(kSignals[i], &action, &g_previous_actions[i]);
  }
}

u64 current_process_id() {
  return getpid();
}

#else

void install_platform_handler() {}

u64 current_process_id() {
  return 0;
}

#endif

}  // namespace

void install() {
  if (g_state) {
    return;
  }
  g_state = new State;
  const char* game = game_version_names[g_game_version];
  g_state->report_path = file_util::get_file_path(
      {"log", fmt::format("{}-crash.{}.{}.txt", game, str_util::current_local_timestamp_no_colons(),
                          current_process_id())});
  file_util::create_dir_if_needed_for_file(g_state->report_path);
  g_state->map_dir = file_util::get_jak_project_dir() / "out" / game / "obj";
  std::thread(reporter_thread).detach();
  install_platform_handler();
}

void set_game_describers(const GameDescribers& describers) {
  g_describers = describers;
}

void note_object_loaded(const char* name, const SegmentInfo* code_infos) {
  for (int seg = 0; seg < N_SEG; seg++) {
    const u32 start = code_infos[seg].offset;
    const u32 size = code_infos[seg].size;
    if (size == 0) {
      continue;
    }
    std::erase_if(g_segments, [&](const LoadedSegment& old) {
      return start < old.start + old.size && old.start < start + size;
    });
    LoadedSegment segment;
    segment.object_name = name;
    segment.seg = seg;
    segment.start = start;
    segment.size = size;
    for (int i = 0; i < N_SEG; i++) {
      segment.object_sizes[i] = code_infos[i].size;
    }
    g_segments.push_back(std::move(segment));
  }
}

bool read_memory(u32 goal_addr, void* out, u32 size) {
  if (goal_addr < u32(EE_MAIN_MEM_LOW_PROTECT) || u64(goal_addr) + size > u64(EE_MAIN_MEM_SIZE)) {
    return false;
  }
  memcpy(out, g_ee_main_mem + goal_addr, size);
  return true;
}

std::string read_c_string(u32 goal_addr) {
  std::string result;
  char c = 0;
  while (int(result.size()) < kMaxStringLength && read_memory(goal_addr + result.size(), &c, 1) &&
         c != 0) {
    result.push_back(c >= 0x20 && c < 0x7f ? c : '?');
  }
  return result;
}

}  // namespace kcrash
