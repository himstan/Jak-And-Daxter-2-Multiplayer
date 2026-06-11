#pragma once
#include <string>

#include "common/util/FileUtil.h"

#include "protocol/common_types.h"

#include "protocol/document_diagnostics.h"

#include <functional>

namespace lsp_util {
std::string url_encode(const std::string& value);
std::string url_decode(const std::string& input);
std::string strip_ansi_escape_codes(const std::string& input);
LSPSpec::DocumentUri uri_from_path(fs::path path);
std::string uri_to_path(const LSPSpec::DocumentUri& uri);

struct ParseResult {
  std::string file_path;
  LSPSpec::Diagnostic diagnostic;
  bool success = false;
};

ParseResult parse_compiler_error(const std::string& error_text);

struct CompileProgressEvent {
  int percentage;
  std::string phase;
  std::string file_path;
  std::optional<double> elapsed_seconds;
};

std::optional<CompileProgressEvent> parse_compile_progress_line(const std::string& line);

class CompileProgressTracker {
 public:
  struct ProgressEvent {
    std::string token;
    std::string kind; // "create", "begin", "report", "end"
    int percentage = -1;
    std::string message;
  };

  using EmitCallback = std::function<void(const ProgressEvent& event)>;

  CompileProgressTracker(const std::string& title, EmitCallback callback, bool use_progress);

  void start(const std::string& start_message);
  void handle_chunk(const std::string& chunk);
  void finish(const std::string& finish_message);

 private:
  void process_line(const std::string& line);

  std::string m_title;
  EmitCallback m_callback;
  bool m_use_progress;
  std::string m_token;
  std::string m_line_buffer;
  int m_last_percentage = -1;
  std::string m_last_file = "";
  bool m_started = false;
};

}  // namespace lsp_util

