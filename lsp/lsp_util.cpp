#include "lsp_util.h"

#include <regex>
#include <sstream>

#include "common/log/log.h"
#include "common/util/string_util.h"

#include "fmt/format.h"

namespace lsp_util {

std::string url_encode(const std::string& value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;

  for (std::string::const_iterator i = value.begin(), n = value.end(); i != n; ++i) {
    std::string::value_type c = (*i);

    // Keep alphanumeric and other accepted characters intact
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      escaped << c;
      continue;
    }

    // Any other characters are percent-encoded
    escaped << std::uppercase;
    escaped << '%' << std::setw(2) << int((unsigned char)c);
    escaped << std::nouppercase;
  }

  return escaped.str();
}

std::string url_decode(const std::string& input) {
  std::ostringstream decoded;

  for (std::size_t i = 0; i < input.length(); ++i) {
    if (input[i] == '%') {
      // Check if there are enough characters remaining
      if (i + 2 < input.length()) {
        // Convert the next two characters after '%' into an integer value
        std::istringstream hexStream(input.substr(i + 1, 2));
        int hexValue = 0;
        hexStream >> std::hex >> hexValue;

        // Append the decoded character to the result
        decoded << static_cast<char>(hexValue);

        // Skip the next two characters
        i += 2;
      }
    } else if (input[i] == '+') {
      // Replace '+' with space character ' '
      decoded << ' ';
    } else {
      // Append the character as is
      decoded << input[i];
    }
  }

  auto result = decoded.str();
  if (input != result) {
    lg::debug("url_decode: {} -> {}", input, result);
  }
  return result;
}

std::string strip_ansi_escape_codes(const std::string& input) {
  static const std::regex ansi_regex(R"(\x1B\[[0-?]*[ -/]*[@-~])");
  return std::regex_replace(input, ansi_regex, "");
}

LSPSpec::DocumentUri uri_from_path(fs::path path) {
  auto path_str = file_util::convert_to_unix_path_separators(path.string());
  // vscode works with proper URL encoded URIs for file paths
  // which means we have to roll our own...
  path_str = url_encode(path_str);
  return fmt::format("file:///{}", path_str);
}

std::string uri_to_path(const LSPSpec::DocumentUri& uri) {
  lg::debug("uri_to_path input: {}", uri);
  auto decoded_uri = url_decode(uri);
  if (str_util::starts_with(decoded_uri, "file:///")) {
#ifdef _WIN32
    decoded_uri = decoded_uri.substr(8);
#else
    decoded_uri = decoded_uri.substr(7);
#endif
  }
  lg::debug("uri_to_path result: {}", decoded_uri);
  return decoded_uri;
}

ParseResult parse_compiler_error(const std::string& error_text) {
  ParseResult result;
  result.success = true;

  std::string stripped_text = strip_ansi_escape_codes(error_text);
  std::vector<std::string> lines = str_util::split_string(stripped_text);
  if (lines.empty()) {
    result.success = false;
    return result;
  }

  result.diagnostic.m_severity = LSPSpec::DiagnosticSeverity::Error;
  result.diagnostic.m_source = "OpenGOAL";
  result.diagnostic.m_range = LSPSpec::Range(0, 0);

  // Check for new "Compilation Error" format
  bool is_compilation_error = false;
  for (const auto& line : lines) {
    if (str_util::contains(line, "-- Compilation Error! --")) {
      is_compilation_error = true;
      break;
    }
  }

  if (is_compilation_error) {
    std::string message;
    std::string form;
    std::string location_path;
    int line_num = 0;
    std::string source_line;
    int caret_col = -1;

    for (size_t i = 0; i < lines.size(); ++i) {
      std::string trimmed = str_util::trim(lines[i]);
      if (str_util::contains(lines[i], "-- Compilation Error! --")) {
        // Message is usually the very next non-empty line
        for (size_t j = i + 1; j < lines.size(); ++j) {
          std::string msg_line = str_util::trim(lines[j]);
          if (!msg_line.empty() && !str_util::contains(msg_line, "goal_src/")) {
            message = msg_line;
            break;
          }
        }
      } else if (trimmed == "Form:") {
        if (i + 1 < lines.size()) {
          form = str_util::trim(lines[i + 1]);
        }
      } else if (trimmed == "Location:") {
        if (i + 1 < lines.size()) {
          std::string loc = str_util::trim(lines[i + 1]);
          // Match path:line, handle Windows drive letters by looking for the last colon
          size_t last_colon = loc.find_last_of(':');
          if (last_colon != std::string::npos && last_colon > 0) {
            location_path = loc.substr(0, last_colon);
            try {
              line_num = std::stoi(loc.substr(last_colon + 1));
            } catch (...) {
            }

            // The lines immediately following Location: are the source code and caret
            // Let's look for them carefully
            for (size_t j = i + 2; j < i + 5 && j < lines.size(); ++j) {
              if (str_util::contains(lines[j], "^")) {
                caret_col = (int)lines[j].find('^');
                if (j > 0) {
                  source_line = lines[j - 1];
                }
                break;
              }
            }
          }
        }
      }
    }

    if (message.empty()) {
      message = "Compilation Error";
    }
    result.diagnostic.m_message = "Compilation Error: " + message;
    result.diagnostic.m_severity = LSPSpec::DiagnosticSeverity::Error;
    result.diagnostic.m_source = "OpenGOAL";

    if (line_num > 0) {
      result.diagnostic.m_range.m_start.m_line = line_num - 1;
      result.diagnostic.m_range.m_end.m_line = line_num - 1;
    }

    // Range calculation priority: Form text match > Caret > Column 0
    if (!form.empty() && !source_line.empty() && source_line.find(form) != std::string::npos) {
      size_t pos = source_line.find(form);
      result.diagnostic.m_range.m_start.m_character = (uint32_t)pos;
      result.diagnostic.m_range.m_end.m_character = (uint32_t)(pos + form.length());
    } else if (caret_col != -1) {
      result.diagnostic.m_range.m_start.m_character = (uint32_t)caret_col;
      result.diagnostic.m_range.m_end.m_character = (uint32_t)(caret_col + 1);
    } else {
      result.diagnostic.m_range.m_start.m_character = 0;
      result.diagnostic.m_range.m_end.m_character = 1;
    }
    } else {
    // Legacy parsing (Reader error, etc.)
    result.diagnostic.m_severity = LSPSpec::DiagnosticSeverity::Error;
    result.diagnostic.m_source = "OpenGOAL";

    std::string error_type = lines[0];

    std::string message_content;
    size_t line_idx = 1;
    while (line_idx < lines.size() && !str_util::starts_with(lines[line_idx], "at ")) {
      if (!message_content.empty()) {
        message_content += " ";
      }
      message_content += str_util::trim(lines[line_idx]);
      line_idx++;
    }

    result.diagnostic.m_message = error_type;
    if (!message_content.empty()) {
      result.diagnostic.m_message += " " + message_content;
    }

    std::regex loc_regex("at (.*):(\\d+)");
    std::smatch loc_match;

    for (size_t i = line_idx; i < lines.size(); i++) {
      if (std::regex_search(lines[i], loc_match, loc_regex)) {
        if (loc_match.size() == 3) {
          result.file_path = loc_match[1].str();
          try {
            int line_num = std::stoi(loc_match[2].str());
            result.diagnostic.m_range.m_start.m_line = line_num - 1;
            result.diagnostic.m_range.m_end.m_line = line_num - 1;

            // Check for caret line in the next few lines
            for (size_t j = i + 1; j < lines.size() && j < i + 4; j++) {
              if (str_util::contains(lines[j], "^")) {
                size_t caret_pos = lines[j].find('^');
                result.diagnostic.m_range.m_start.m_character = (uint32_t)caret_pos;
                result.diagnostic.m_range.m_end.m_character = (uint32_t)caret_pos + 1;
                break;
              }
            }
          } catch (...) {
          }
          break;
        }
      }
    }

    // Fallback range if not found
    if (result.diagnostic.m_range.m_end.m_character == 0) {
      result.diagnostic.m_range.m_end.m_character = 1;
    }
  }

  return result;
}

std::optional<CompileProgressEvent> parse_compile_progress_line(const std::string& line) {
  std::string stripped = strip_ansi_escape_codes(line);

  // Regex for goalc progress lines:
  // [  2%] [goalc   ] 0.050 goal_src/jak2/multiplayer/core/mp-types.gc
  // [  2%] [goalc   ]       goal_src/jak2/engine/math/euler-h.gc
  //
  // Group 1: percentage
  // Group 2: phase (e.g. goalc)
  // Group 3: optional elapsed time
  // Group 4: file path
  static const std::regex progress_regex(
      R"(\[\s*(\d+)%\]\s*\[\s*(\w+)\s*\]\s*(\d+\.\d+)?\s*(.*))");

  std::smatch match;
  if (std::regex_search(stripped, match, progress_regex)) {
    CompileProgressEvent event;
    event.percentage = std::stoi(match[1].str());
    event.phase = match[2].str();
    if (match[3].matched) {
      event.elapsed_seconds = std::stod(match[3].str());
    }
    event.file_path = str_util::trim(match[4].str());
    
    // Sometimes the "file path" might be just spaces if it's a line that doesn't have it
    // but the regex above will capture it. Let's make sure it's not empty.
    if (event.file_path.empty()) {
      return std::nullopt;
    }

    return event;
  }

  return std::nullopt;
}

CompileProgressTracker::CompileProgressTracker(const std::string& title,
                                               EmitCallback callback,
                                               bool use_progress)
    : m_title(title), m_callback(callback), m_use_progress(use_progress) {
  m_token = fmt::format("opengoal/{}/{}", m_title, str_util::uuid());
}

void CompileProgressTracker::start(const std::string& start_message) {
  if (!m_use_progress) return;
  m_started = true;
  m_last_percentage = 0;
  
  ProgressEvent create_ev;
  create_ev.token = m_token;
  create_ev.kind = "create";
  create_ev.message = start_message;
  m_callback(create_ev);

  ProgressEvent begin_ev;
  begin_ev.token = m_token;
  begin_ev.kind = "begin";
  begin_ev.percentage = 0;
  begin_ev.message = start_message;
  m_callback(begin_ev);
}

void CompileProgressTracker::handle_chunk(const std::string& chunk) {
  if (!m_use_progress || !m_started) return;
  m_line_buffer += chunk;
  
  size_t pos = 0;
  while (true) {
    size_t next_nl = m_line_buffer.find('\n', pos);
    size_t next_cr = m_line_buffer.find('\r', pos);
    size_t found = std::string::npos;
    if (next_nl != std::string::npos && next_cr != std::string::npos) {
      found = std::min(next_nl, next_cr);
    } else if (next_nl != std::string::npos) {
      found = next_nl;
    } else if (next_cr != std::string::npos) {
      found = next_cr;
    }
    
    if (found == std::string::npos) {
      break;
    }
    
    std::string line = m_line_buffer.substr(pos, found - pos);
    process_line(line);
    pos = found + 1;
  }
  if (pos > 0) {
    m_line_buffer = m_line_buffer.substr(pos);
  }
}

void CompileProgressTracker::finish(const std::string& finish_message) {
  if (!m_use_progress || !m_started) return;
  
  // First process any remaining text in the buffer if it exists
  if (!m_line_buffer.empty()) {
    process_line(m_line_buffer);
    m_line_buffer.clear();
  }

  ProgressEvent end_ev;
  end_ev.token = m_token;
  end_ev.kind = "end";
  end_ev.message = finish_message;
  m_callback(end_ev);

  m_started = false;
}

void CompileProgressTracker::process_line(const std::string& line) {
  auto event = parse_compile_progress_line(line);
  if (event) {
    if (event->percentage != m_last_percentage) {
      ProgressEvent p_event;
      p_event.token = m_token;
      p_event.kind = "report";
      p_event.percentage = event->percentage;
      p_event.message = event->file_path;
      m_callback(p_event);
      
      m_last_percentage = event->percentage;
      m_last_file = event->file_path;
    }
  }
}

}  // namespace lsp_util


