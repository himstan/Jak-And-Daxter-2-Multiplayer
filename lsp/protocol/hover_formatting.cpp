#include "hover_formatting.h"

#include <algorithm>
#include <sstream>

#include "common/util/string_util.h"
#include "fmt/format.h"

namespace lsp_hover {
namespace {

std::string escape_markdown_text(const std::string& text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '\\':
      case '*':
      case '_':
      case '<':
      case '>':
      case '|':
      case '`':
      case '[':
      case ']':
        escaped.push_back('\\');
        escaped.push_back(ch);
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

std::string code_span(const std::string& text) {
  return fmt::format("`{}`", text);
}

std::string normalize_type(const std::string& type) {
  return type.empty() ? "unknown" : type;
}

std::string kind_label(const HoverKind kind) {
  switch (kind) {
    case HoverKind::Function:
      return "Function";
    case HoverKind::Method:
      return "Method";
    case HoverKind::Macro:
      return "Macro";
    case HoverKind::Global:
      return "Global variable";
    case HoverKind::Parameter:
      return "Parameter";
    case HoverKind::Local:
      return "Local variable";
    case HoverKind::Field:
      return "Field";
    case HoverKind::Type:
      return "Type";
    case HoverKind::Constant:
      return "Constant";
    case HoverKind::EnumValue:
      return "Enum value";
    case HoverKind::Unknown:
    default:
      return "Symbol";
  }
}

std::string summarize_docstring(const std::string& docstring) {
  if (docstring.empty()) {
    return "";
  }

  std::string summary;
  const auto lines = str_util::split(docstring);
  for (const auto& line : lines) {
    const auto trimmed_line = str_util::trim(line);
    if (trimmed_line.empty() || str_util::starts_with(trimmed_line, "@")) {
      break;
    }
    if (!summary.empty()) {
      summary += "\n";
    }
    summary += escape_markdown_text(trimmed_line);
  }
  return summary;
}

std::string signature_for_hover(const HoverInfo& info) {
  if (!info.signature.empty()) {
    return info.signature;
  }

  const auto normalized_type = normalize_type(info.type);
  switch (info.kind) {
    case HoverKind::Parameter:
    case HoverKind::Local:
    case HoverKind::Global:
    case HoverKind::Constant:
    case HoverKind::EnumValue:
      return fmt::format("{}: {}", info.name, normalized_type);
    case HoverKind::Field:
      return fmt::format("{}: {}", info.name, normalized_type);
    case HoverKind::Type: {
      if (!info.parent_type.empty()) {
        return fmt::format("(deftype {} ...)", info.name);
      }
      return fmt::format("(deftype {} ...)", info.name);
    }
    case HoverKind::Macro:
      return fmt::format("({} ...)", info.name);
    case HoverKind::Method: {
      std::string signature = fmt::format("(defmethod {} ", info.name);
      std::vector<std::string> arg_forms;
      if (!info.owner_type.empty()) {
        arg_forms.push_back(fmt::format("(self {})", info.owner_type));
      }
      for (const auto& param : info.params) {
        arg_forms.push_back(fmt::format("({} {})", param.name, normalize_type(param.type)));
      }
      signature += "(";
      for (size_t i = 0; i < arg_forms.size(); ++i) {
        if (i > 0) {
          signature += " ";
        }
        signature += arg_forms[i];
      }
      signature += ")";
      if (!info.return_type.empty()) {
        signature += fmt::format(" -> {}", info.return_type);
      }
      signature += ")";
      return signature;
    }
    case HoverKind::Function:
    case HoverKind::Unknown:
    default: {
      if (info.params.empty()) {
        std::string signature = fmt::format("(defun {} ()", info.name);
        if (!info.return_type.empty()) {
          signature += fmt::format(" -> {}", info.return_type);
        }
        signature += ")";
        return signature;
      }

      std::string signature = fmt::format("(defun {} (", info.name);
      for (size_t i = 0; i < info.params.size(); ++i) {
        if (i > 0) {
          signature += " ";
        }
        signature += fmt::format("({} {})", info.params[i].name, normalize_type(info.params[i].type));
      }
      signature += ")";
      if (!info.return_type.empty()) {
        signature += fmt::format(" -> {}", info.return_type);
      }
      signature += ")";
      return signature;
    }
  }
}

void append_section(std::string& output, const std::string& title) {
  if (!output.empty()) {
    output += "\n";
  }
  output += fmt::format("**{}**", title);
}

void append_params(std::string& output, const std::vector<HoverParam>& params) {
  if (params.empty()) {
    return;
  }

  if (!output.empty()) {
    output += "\n";
  }
  output += "**Parameters**\n";
  for (const auto& param : params) {
    output += fmt::format("* {}: {}\n", code_span(param.name), code_span(normalize_type(param.type)));
  }
  if (!output.empty() && output.back() == '\n') {
    output.pop_back();
  }
}

void append_fields(std::string& output, const std::vector<HoverParam>& fields) {
  if (fields.empty()) {
    return;
  }

  if (!output.empty()) {
    output += "\n";
  }
  output += "**Fields**\n";
  for (const auto& field : fields) {
    output += fmt::format("* {}: {}\n", code_span(field.name), code_span(normalize_type(field.type)));
  }
  if (!output.empty() && output.back() == '\n') {
    output.pop_back();
  }
}

void append_returns(std::string& output, const std::string& return_type) {
  if (return_type.empty()) {
    return;
  }

  if (!output.empty()) {
    output += "\n";
  }
  output += fmt::format("**Returns** {}", code_span(return_type));
}

}  // namespace

std::string format_hover_markdown(const HoverInfo& info) {
  std::string markdown;
  markdown += fmt::format("```goal\n{}\n```\n\n", signature_for_hover(info));
  append_section(markdown, kind_label(info.kind));

  if (info.kind == HoverKind::Field && !info.owner_type.empty()) {
    markdown += fmt::format(" of {}", code_span(info.owner_type));
  }

  const auto doc_summary = summarize_docstring(info.docstring);
  if (!doc_summary.empty()) {
    markdown += "\n\n" + doc_summary;
  }

  if (info.kind == HoverKind::Type && !info.parent_type.empty()) {
    markdown += "\n\n";
    markdown += fmt::format("**Extends** {}", code_span(info.parent_type));
  }

  if (info.kind == HoverKind::Function || info.kind == HoverKind::Method) {
    append_params(markdown, info.params);
    append_returns(markdown, info.return_type);
  } else if (info.kind == HoverKind::Type) {
    append_fields(markdown, info.fields);
  }

  return markdown;
}

LSPSpec::Hover make_markdown_hover(const HoverInfo& info) {
  LSPSpec::Hover hover;
  hover.m_contents.m_kind = "markdown";
  hover.m_contents.m_value = format_hover_markdown(info);
  return hover;
}

}  // namespace lsp_hover
