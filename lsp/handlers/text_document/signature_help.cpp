#include "signature_help.h"

#include <algorithm>
#include <string>
#include <vector>

#include "common/log/log.h"
#include "common/util/ast_util.h"
#include "goalc/compiler/symbol_info.h"

namespace {

struct CallSignatureContext {
  bool valid = false;
  std::string call_name;
  int active_argument_index = 0;
  TSNode call_node = {{0, 0, 0, 0}};
};

struct SignatureParts {
  bool valid = false;
  std::string resolved_kind;
  std::vector<symbol_info::ArgumentInfo> params;
  std::string return_type = "none";
  std::string owner_type;
  std::string docstring;
};

enum class LocalSignaturePreference {
  DefbehaviorOnly,
  DefineExternOnly,
  Any,
};

bool is_list_like_node(const std::string& node_type) {
  return node_type == "list" || node_type == "form" || node_type == "list_lit" ||
         node_type == "form_lit";
}

bool is_symbol_node(TSNode node) {
  if (ts_node_is_null(node)) {
    return false;
  }
  const std::string node_type = ts_node_type(node);
  return node_type == "sym_name" || node_type == "sym_lit";
}

LSPSpec::Position node_start(TSNode node) {
  const auto point = ts_node_start_point(node);
  return {point.row, point.column};
}

LSPSpec::Position node_end(TSNode node) {
  const auto point = ts_node_end_point(node);
  return {point.row, point.column};
}

bool position_before(const LSPSpec::Position& lhs, const LSPSpec::Position& rhs) {
  return lhs.m_line < rhs.m_line ||
         (lhs.m_line == rhs.m_line && lhs.m_character < rhs.m_character);
}

bool position_after(const LSPSpec::Position& lhs, const LSPSpec::Position& rhs) {
  return lhs.m_line > rhs.m_line ||
         (lhs.m_line == rhs.m_line && lhs.m_character > rhs.m_character);
}

bool position_after_or_equal(const LSPSpec::Position& lhs, const LSPSpec::Position& rhs) {
  return position_after(lhs, rhs) ||
         (lhs.m_line == rhs.m_line && lhs.m_character == rhs.m_character);
}

bool position_within_node(const LSPSpec::Position& position, TSNode node) {
  if (ts_node_is_null(node)) {
    return false;
  }
  return position_after_or_equal(position, node_start(node)) &&
         !position_after(position, node_end(node));
}

std::string node_text(const WorkspaceOGFile& file, TSNode node) {
  if (ts_node_is_null(node)) {
    return "";
  }
  return ast_util::get_source_code(file.m_content, node);
}

std::vector<TSNode> named_children(TSNode node) {
  std::vector<TSNode> children;
  if (ts_node_is_null(node)) {
    return children;
  }
  const uint32_t count = ts_node_named_child_count(node);
  children.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    children.push_back(ts_node_named_child(node, i));
  }
  return children;
}

std::string trim_copy(const std::string& input) {
  const auto first = input.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = input.find_last_not_of(" \t\r\n");
  return input.substr(first, last - first + 1);
}

std::string strip_outer_parens(const std::string& input) {
  std::string trimmed = trim_copy(input);
  if (trimmed.size() >= 2 && trimmed.front() == '(' && trimmed.back() == ')') {
    return trim_copy(trimmed.substr(1, trimmed.size() - 2));
  }
  return trimmed;
}

std::vector<std::string> split_top_level_tokens(const std::string& input) {
  std::vector<std::string> tokens;
  std::string current;
  int depth = 0;
  for (char c : input) {
    if ((c == ' ' || c == '\t' || c == '\r' || c == '\n') && depth == 0) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    if (c == '(') {
      depth++;
    } else if (c == ')' && depth > 0) {
      depth--;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

SignatureParts signature_from_function_type_text(const std::string& type_text) {
  SignatureParts parts;
  const auto tokens = split_top_level_tokens(strip_outer_parens(type_text));
  if (tokens.empty() || tokens.front() != "function") {
    return parts;
  }

  auto behavior_marker = std::find(tokens.begin(), tokens.end(), ":behavior");
  if (behavior_marker != tokens.end()) {
    parts.valid = true;
    parts.resolved_kind = "behavior";
    parts.return_type = "none";
    if (std::next(behavior_marker) != tokens.end()) {
      parts.owner_type = *std::next(behavior_marker);
      symbol_info::ArgumentInfo self;
      self.name = "self";
      self.type = parts.owner_type;
      parts.params.push_back(self);
    }
    return parts;
  }

  if (tokens.size() < 2) {
    return parts;
  }

  parts.valid = true;
  parts.resolved_kind = "function";
  parts.return_type = tokens.back();
  for (size_t i = 1; i + 1 < tokens.size(); ++i) {
    symbol_info::ArgumentInfo arg;
    arg.name = "arg" + std::to_string(i - 1);
    arg.type = tokens[i];
    parts.params.push_back(arg);
  }
  return parts;
}

std::optional<SignatureParts> local_signature_from_node(const WorkspaceOGFile& file,
                                                        TSNode node,
                                                        const std::string& call_name,
                                                        LocalSignaturePreference preference) {
  if (!is_list_like_node(ts_node_type(node))) {
    return {};
  }

  const auto children = named_children(node);
  if (children.empty() || !is_symbol_node(children.front())) {
    return {};
  }

  const auto head = node_text(file, children.front());
  if (head == "defbehavior" &&
      preference != LocalSignaturePreference::DefineExternOnly &&
      children.size() >= 3 && node_text(file, children[1]) == call_name) {
    SignatureParts parts;
    parts.valid = true;
    parts.resolved_kind = "behavior";
    parts.owner_type = node_text(file, children[2]);
    parts.return_type = "none";
    return parts;
  }

  if (head == "define-extern" &&
      preference != LocalSignaturePreference::DefbehaviorOnly &&
      children.size() >= 3 && node_text(file, children[1]) == call_name) {
    auto parts = signature_from_function_type_text(node_text(file, children[2]));
    if (parts.valid) {
      return parts;
    }
  }

  return {};
}

std::optional<SignatureParts> local_signature_from_file(const WorkspaceOGFile& file,
                                                        TSNode node,
                                                        const std::string& call_name,
                                                        LocalSignaturePreference preference) {
  auto local = local_signature_from_node(file, node, call_name, preference);
  if (local) {
    return local;
  }

  const uint32_t child_count = ts_node_child_count(node);
  for (uint32_t i = 0; i < child_count; ++i) {
    auto child_local =
        local_signature_from_file(file, ts_node_child(node, i), call_name, preference);
    if (child_local) {
      return child_local;
    }
  }
  return {};
}

std::optional<SignatureParts> local_signature_from_file(const WorkspaceOGFile& file,
                                                        const std::string& call_name,
                                                        LocalSignaturePreference preference) {
  if (!file.get_ast()) {
    return {};
  }
  return local_signature_from_file(file, ts_tree_root_node(file.get_ast().get()), call_name,
                                   preference);
}

TSNode list_containing_cursor(const WorkspaceOGFile& file, const LSPSpec::Position& cursor) {
  LSPSpec::Position probe = cursor;
  if (probe.m_character > 0) {
    probe.m_character--;
  }

  TSNode node = file.get_node_at_position(probe);
  while (!ts_node_is_null(node)) {
    if (is_list_like_node(ts_node_type(node)) && position_within_node(cursor, node)) {
      return node;
    }
    node = ts_node_parent(node);
  }
  return {{0, 0, 0, 0}};
}

CallSignatureContext detect_call_signature_context(const WorkspaceOGFile& file,
                                                   const LSPSpec::Position& cursor) {
  CallSignatureContext context;
  TSNode current = list_containing_cursor(file, cursor);

  while (!ts_node_is_null(current)) {
    auto children = named_children(current);
    if (children.empty() || !is_symbol_node(children.front())) {
      current = ts_node_parent(current);
      continue;
    }

    TSNode head = children.front();
    if (position_before(cursor, node_end(head))) {
      current = ts_node_parent(current);
      continue;
    }

    context.valid = true;
    context.call_name = node_text(file, head);
    context.call_node = current;
    context.active_argument_index = 0;

    for (size_t i = 1; i < children.size(); ++i) {
      TSNode child = children[i];
      if (position_before(cursor, node_start(child))) {
        break;
      }
      if (position_within_node(cursor, child)) {
        context.active_argument_index = static_cast<int>(i - 1);
        return context;
      }
      if (!position_after(cursor, node_end(child))) {
        context.active_argument_index = static_cast<int>(i - 1);
        return context;
      }
      context.active_argument_index = static_cast<int>(i);
    }
    return context;
  }

  return context;
}

std::string type_spec_arg_label(const TypeSpec& type_spec) {
  return type_spec.arg_count() == 0 ? type_spec.base_type() : type_spec.print();
}

SignatureParts signature_from_type_spec(const TypeSpec& type_spec) {
  SignatureParts parts;
  if (type_spec.base_type() != "function" || type_spec.arg_count() == 0) {
    return parts;
  }

  parts.valid = true;
  parts.resolved_kind = "function";
  const int arg_count = type_spec.arg_count();
  parts.return_type = type_spec_arg_label(type_spec.get_arg(arg_count - 1));
  for (int i = 0; i < arg_count - 1; ++i) {
    symbol_info::ArgumentInfo arg;
    arg.name = "arg" + std::to_string(i);
    arg.type = type_spec_arg_label(type_spec.get_arg(i));
    parts.params.push_back(arg);
  }
  return parts;
}

SignatureParts signature_from_symbol(const symbol_info::SymbolInfo& info,
                                     const std::string& call_name,
                                     const WorkspaceOGFile& file,
                                     Workspace& workspace) {
  SignatureParts parts;
  parts.docstring = info.m_docstring;

  switch (info.m_kind) {
    case symbol_info::Kind::FUNCTION:
    case symbol_info::Kind::LANGUAGE_BUILTIN:
      parts.valid = true;
      parts.resolved_kind = "function";
      parts.params = info.m_args;
      parts.return_type = info.m_return_type.empty() ? "none" : info.m_return_type;
      return parts;
    case symbol_info::Kind::METHOD:
      parts.valid = true;
      parts.resolved_kind = "method";
      parts.owner_type = info.m_method_info.type_name;
      parts.params = info.m_args;
      parts.return_type = info.m_method_info.type.arg_count() > 0
                              ? type_spec_arg_label(info.m_method_info.type.last_arg())
                              : "none";
      return parts;
    case symbol_info::Kind::STATE:
      parts.valid = true;
      parts.resolved_kind = "behavior";
      parts.owner_type = info.m_state_related_type;
      parts.params = info.m_state_enter_and_code_args;
      parts.return_type = "none";
      return parts;
    case symbol_info::Kind::MACRO:
      parts.valid = true;
      parts.resolved_kind = "macro";
      for (const auto& macro_arg : info.m_macro_args) {
        symbol_info::ArgumentInfo arg;
        arg.name = macro_arg;
        arg.type = "form";
        parts.params.push_back(arg);
      }
      if (info.m_variadic_arg) {
        symbol_info::ArgumentInfo arg;
        arg.name = info.m_variadic_arg.value();
        arg.type = "form...";
        parts.params.push_back(arg);
      }
      parts.return_type = "form";
      return parts;
    case symbol_info::Kind::FWD_DECLARED_SYM: {
      auto type_info = workspace.get_symbol_typeinfo(file, call_name);
      if (type_info) {
        return signature_from_type_spec(type_info->first);
      }
      return parts;
    }
    default:
      return parts;
  }
}

std::string parameter_label(const symbol_info::ArgumentInfo& arg) {
  if (arg.type.empty()) {
    return arg.name;
  }
  return arg.name + ": " + arg.type;
}

LSPSpec::SignatureHelp empty_signature_help() {
  return {};
}

LSPSpec::SignatureHelp make_help(const std::string& call_name,
                                 const CallSignatureContext& context,
                                 const SignatureParts& parts) {
  LSPSpec::SignatureInformation signature;
  std::string display_name = call_name;
  if (parts.resolved_kind == "behavior") {
    display_name = "behavior " + call_name;
    if (!parts.owner_type.empty()) {
      display_name += " " + parts.owner_type;
    }
  } else if (parts.resolved_kind == "method" && !parts.owner_type.empty()) {
    display_name = parts.owner_type + "." + call_name;
  }

  signature.m_label = display_name + "(";
  for (size_t i = 0; i < parts.params.size(); ++i) {
    if (i > 0) {
      signature.m_label += ", ";
    }
    const auto label = parameter_label(parts.params[i]);
    signature.m_label += label;

    LSPSpec::ParameterInformation param;
    param.m_label = label;
    if (!parts.params[i].description.empty()) {
      param.m_documentation = LSPSpec::MarkupContent{"markdown", parts.params[i].description};
    }
    signature.m_parameters.push_back(param);
  }
  signature.m_label += "): " + parts.return_type;
  if (!parts.docstring.empty()) {
    signature.m_documentation = LSPSpec::MarkupContent{"markdown", parts.docstring};
  }

  LSPSpec::SignatureHelp help;
  help.m_signatures.push_back(signature);
  help.m_activeSignature = 0;
  if (!parts.params.empty()) {
    const int max_index = static_cast<int>(parts.params.size()) - 1;
    const auto active =
        static_cast<uint32_t>(std::clamp(context.active_argument_index, 0, max_index));
    help.m_activeParameter = active;
    help.m_signatures.front().m_activeParameter = active;
  }
  return help;
}

}  // namespace

namespace lsp_handlers {

std::optional<json> signature_help(Workspace& workspace, json /*id*/, json raw_params) {
  auto params = raw_params.get<LSPSpec::SignatureHelpParams>();
  workspace.ensure_file_tracked(params.m_textDocument.m_uri);
  if (workspace.determine_filetype_from_uri(params.m_textDocument.m_uri) !=
      Workspace::FileType::OpenGOAL) {
    return json(empty_signature_help());
  }

  auto maybe_file = workspace.get_tracked_og_file(params.m_textDocument.m_uri);
  if (!maybe_file) {
    return json(empty_signature_help());
  }
  const auto& file = maybe_file.value().get();

  auto context = detect_call_signature_context(file, params.m_position);
  if (!context.valid || context.call_name.empty()) {
    return json(empty_signature_help());
  }

  auto local_behavior =
      local_signature_from_file(file, context.call_name, LocalSignaturePreference::DefbehaviorOnly);
  std::optional<SignatureParts> maybe_parts = local_behavior;

  auto symbol = workspace.get_global_symbol_info(file, context.call_name);
  if (!maybe_parts && symbol) {
    maybe_parts = signature_from_symbol(**symbol, context.call_name, file, workspace);
  }

  if (!maybe_parts || !maybe_parts->valid) {
    maybe_parts =
        local_signature_from_file(file, context.call_name, LocalSignaturePreference::DefineExternOnly);
  }

  if (!maybe_parts || !maybe_parts->valid) {
    lg::debug("signatureHelp: call={} unresolved", context.call_name);
    return json(empty_signature_help());
  }

  auto parts = maybe_parts.value();

  lg::debug("signatureHelp: call={} activeArg={} resolved={} params={} return={}",
            context.call_name, context.active_argument_index, parts.resolved_kind,
            parts.params.size(), parts.return_type);
  return json(make_help(context.call_name, context, parts));
}

}  // namespace lsp_handlers
