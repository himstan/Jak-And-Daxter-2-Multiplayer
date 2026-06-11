#include "completion.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "common/log/log.h"
#include "common/util/ast_util.h"
#include "common/versions/versions.h"

namespace {

struct FieldCompletionContext {
  bool valid = false;
  std::string operator_name;
  TSNode receiver_node = {{0, 0, 0, 0}};
  std::vector<std::string> completed_field_chain;
  std::string current_prefix;
};

std::unordered_map<symbol_info::Kind, LSPSpec::CompletionItemKind> completion_item_kind_map = {
    {symbol_info::Kind::CONSTANT, LSPSpec::CompletionItemKind::Constant},
    {symbol_info::Kind::FUNCTION, LSPSpec::CompletionItemKind::Function},
    {symbol_info::Kind::FWD_DECLARED_SYM, LSPSpec::CompletionItemKind::Reference},
    {symbol_info::Kind::GLOBAL_VAR, LSPSpec::CompletionItemKind::Variable},
    {symbol_info::Kind::INVALID, LSPSpec::CompletionItemKind::Text},
    {symbol_info::Kind::LANGUAGE_BUILTIN, LSPSpec::CompletionItemKind::Function},
    {symbol_info::Kind::MACRO, LSPSpec::CompletionItemKind::Operator},
    {symbol_info::Kind::METHOD, LSPSpec::CompletionItemKind::Method},
    {symbol_info::Kind::TYPE, LSPSpec::CompletionItemKind::Class},
};

bool is_list_like_node(const std::string& node_type) {
  return node_type == "list" || node_type == "form" || node_type == "list_lit" ||
         node_type == "form_lit";
}

bool is_symbol_node(const TSNode node) {
  if (ts_node_is_null(node)) {
    return false;
  }
  const std::string node_type = ts_node_type(node);
  return node_type == "sym_name" || node_type == "sym_lit";
}

TSNode normalize_symbol_node(TSNode node) {
  if (!ts_node_is_null(node) && std::string(ts_node_type(node)) == "sym_name") {
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent) && std::string(ts_node_type(parent)) == "sym_lit") {
      return parent;
    }
  }
  return node;
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

bool position_within_node(const LSPSpec::Position& position, const TSNode node) {
  if (ts_node_is_null(node)) {
    return false;
  }

  const auto start = ts_node_start_point(node);
  const auto end = ts_node_end_point(node);
  const LSPSpec::Position start_pos{start.row, start.column};
  const LSPSpec::Position end_pos{end.row, end.column};
  return position_after_or_equal(position, start_pos) && !position_after(position, end_pos);
}

std::vector<TSNode> get_named_children(const TSNode node) {
  std::vector<TSNode> children;
  if (ts_node_is_null(node)) {
    return children;
  }

  const uint32_t child_count = ts_node_named_child_count(node);
  children.reserve(child_count);
  for (uint32_t i = 0; i < child_count; ++i) {
    children.push_back(ts_node_named_child(node, i));
  }
  return children;
}

std::string get_node_text(const WorkspaceOGFile& file, const TSNode node) {
  if (ts_node_is_null(node)) {
    return "";
  }
  return ast_util::get_source_code(file.m_content, node);
}

std::string get_prefix_text(const WorkspaceOGFile& file,
                            const TSNode node,
                            const LSPSpec::Position& cursor) {
  if (ts_node_is_null(node)) {
    return "";
  }

  const auto start = ts_node_start_point(node);
  if (cursor.m_line != start.row || cursor.m_character < start.column) {
    return "";
  }

  const std::string text = get_node_text(file, node);
  const uint32_t prefix_len = std::min<uint32_t>(cursor.m_character - start.column,
                                                  static_cast<uint32_t>(text.size()));
  return text.substr(0, prefix_len);
}

std::string join_chain(const std::vector<std::string>& chain) {
  std::string result;
  for (size_t i = 0; i < chain.size(); ++i) {
    if (i > 0) {
      result += ", ";
    }
    result += chain[i];
  }
  return result;
}

std::optional<std::string> infer_expr_type(const WorkspaceOGFile& file,
                                           const TSNode node,
                                           Workspace& workspace) {
  const std::string inferred = infer_type(file, node, workspace);
  if (inferred.empty()) {
    return {};
  }
  return inferred;
}

std::optional<std::string> resolve_field_chain_type(const std::string& receiver_type,
                                                    const std::vector<std::string>& fields,
                                                    const WorkspaceOGFile& file,
                                                    Workspace& workspace) {
  if (receiver_type.empty()) {
    return {};
  }

  std::string current_type = receiver_type;
  for (const auto& field_name : fields) {
    auto field_info = workspace.get_field_info(file, current_type, field_name);
    if (!field_info) {
      return {};
    }
    current_type = field_info->type;
    if (current_type.empty()) {
      return {};
    }
  }

  return current_type;
}

FieldCompletionContext detect_field_completion_context(const WorkspaceOGFile& file,
                                                       const LSPSpec::Position& cursor_position) {
  FieldCompletionContext context;

  LSPSpec::Position probe_position = cursor_position;
  if (probe_position.m_character > 0) {
    probe_position.m_character--;
  }

  TSNode node = file.get_node_at_position(probe_position);
  while (!ts_node_is_null(node)) {
    if (is_list_like_node(ts_node_type(node))) {
      const auto named_children = get_named_children(node);
      if (named_children.size() >= 2 && is_symbol_node(named_children[0])) {
        const std::string operator_name = get_node_text(file, named_children[0]);
        if (operator_name == "->" || operator_name == "&->") {
          const TSNode receiver_node = named_children[1];
          const auto receiver_end = ts_node_end_point(receiver_node);
          const LSPSpec::Position receiver_end_pos{receiver_end.row, receiver_end.column};

          if (!position_after(cursor_position, receiver_end_pos)) {
            return context;
          }

          context.valid = true;
          context.operator_name = operator_name;
          context.receiver_node = receiver_node;

          for (size_t i = 2; i < named_children.size(); ++i) {
            const TSNode field_node = named_children[i];
            const auto field_start = ts_node_start_point(field_node);
            const auto field_start_pos = LSPSpec::Position{field_start.row, field_start.column};

            if (position_before(cursor_position, field_start_pos)) {
              break;
            }

            if (position_within_node(cursor_position, field_node)) {
              context.current_prefix = get_prefix_text(file, field_node, cursor_position);
              return context;
            }

            context.completed_field_chain.push_back(get_node_text(file, field_node));
          }

          return context;
        }
      }
    }
    node = ts_node_parent(node);
  }

  return context;
}

LSPSpec::TextEdit make_text_edit(const LSPSpec::Position& cursor_position,
                                 const std::string& current_prefix,
                                 const std::string& new_text) {
  LSPSpec::Position start_position = cursor_position;
  if (start_position.m_character >= current_prefix.size()) {
    start_position.m_character -= static_cast<uint32_t>(current_prefix.size());
  } else {
    start_position.m_character = 0;
  }

  LSPSpec::TextEdit text_edit;
  text_edit.range = LSPSpec::Range(start_position, cursor_position);
  text_edit.newText = new_text;
  return text_edit;
}

LSPSpec::CompletionItem make_field_completion_item(const symbol_info::FieldInfo& field,
                                                   const LSPSpec::Position& cursor_position,
                                                   const std::string& current_prefix,
                                                   const std::string& owner_type) {
  LSPSpec::CompletionItem item;
  item.label = field.name;
  item.kind = LSPSpec::CompletionItemKind::Field;
  item.detail = field.type;
  if (!field.description.empty()) {
    item.documentation = field.description;
  } else {
    item.documentation = "field of " + owner_type;
  }
  item.sortText = "0_" + field.name;
  item.filterText = field.name;
  item.insertText = field.name;
  item.textEdit = make_text_edit(cursor_position, current_prefix, field.name);
  return item;
}

std::optional<json> complete_field_access(Workspace& workspace,
                                         const WorkspaceOGFile& tracked_file,
                                         const LSPSpec::Position& cursor_position) {
  const auto context = detect_field_completion_context(tracked_file, cursor_position);
  if (!context.valid) {
    return {};
  }

  auto receiver_type = infer_expr_type(tracked_file, context.receiver_node, workspace);
  if (!receiver_type) {
    const std::string receiver_name = get_node_text(tracked_file, context.receiver_node);
    auto type_info = workspace.get_symbol_typeinfo(tracked_file, receiver_name);
    if (type_info) {
      receiver_type = type_info->first.base_type();
    }
  }

  if (!receiver_type || receiver_type->empty()) {
    lg::debug("field completion: receiver={} receiver_type=<unknown> chain=[{}] prefix=\"{}\" suggestions=0",
              get_node_text(tracked_file, context.receiver_node),
              join_chain(context.completed_field_chain),
              context.current_prefix);
    return LSPSpec::CompletionList{false, {}};
  }

  auto resolved_type =
      resolve_field_chain_type(*receiver_type, context.completed_field_chain, tracked_file, workspace);
  if (!resolved_type || resolved_type->empty()) {
    lg::debug("field completion: receiver={} receiver_type={} chain=[{}] prefix=\"{}\" suggestions=0",
              get_node_text(tracked_file, context.receiver_node),
              *receiver_type,
              join_chain(context.completed_field_chain),
              context.current_prefix);
    return LSPSpec::CompletionList{false, {}};
  }

  const auto fields = workspace.get_field_suggestions(tracked_file, *resolved_type);
  std::vector<LSPSpec::CompletionItem> items;
  items.reserve(fields.size());
  for (const auto& field : fields) {
    items.push_back(
        make_field_completion_item(field, cursor_position, context.current_prefix, *resolved_type));
  }

  lg::debug("field completion: receiver={} receiver_type={} chain=[{}] current_type={} prefix=\"{}\" suggestions={}",
            get_node_text(tracked_file, context.receiver_node),
            *receiver_type,
            join_chain(context.completed_field_chain),
            *resolved_type,
            context.current_prefix,
            items.size());

  LSPSpec::CompletionList list_result;
  list_result.isIncomplete = false;
  list_result.items = std::move(items);
  return list_result;
}

}  // namespace

namespace lsp_handlers {

std::optional<json> get_completions(Workspace& workspace, json /*id*/, json params) {
  auto converted_params = params.get<LSPSpec::CompletionParams>();
  workspace.ensure_file_tracked(converted_params.textDocument.m_uri);
  const auto file_type = workspace.determine_filetype_from_uri(converted_params.textDocument.m_uri);

  if (file_type != Workspace::FileType::OpenGOAL) {
    lg::debug("get_completions - unsupported file type: {}", (int)file_type);
    return LSPSpec::CompletionList{false, {}};
  }

  auto maybe_tracked_file = workspace.get_tracked_og_file(converted_params.textDocument.m_uri);
  if (!maybe_tracked_file) {
    lg::debug("get_completions - file not tracked: {}", converted_params.textDocument.m_uri);
    return LSPSpec::CompletionList{false, {}};
  }

  const auto& tracked_file = maybe_tracked_file.value().get();
  if (!workspace.is_compiler_ready(tracked_file.m_game_version)) {
    lg::debug("get_completions - compiler not ready for {}",
              version_to_game_name(tracked_file.m_game_version));
    return LSPSpec::CompletionList{false, {}};
  }

  if (auto field_completion = complete_field_access(workspace, tracked_file, converted_params.position)) {
    return field_completion;
  }

  // The cursor position in the context of completions is always 1 character ahead of the text, we
  // move it back 1 spot so we can actually detect what the user has typed so far.
  LSPSpec::Position new_position = converted_params.position;
  if (new_position.m_character > 0) {
    new_position.m_character--;
  }

  const auto symbol = tracked_file.get_symbol_at_position(new_position);
  if (!symbol) {
    lg::debug("get_completions - no symbol to work from at {}:{}",
              new_position.m_line,
              new_position.m_character);
    return LSPSpec::CompletionList{false, {}};
  }

  const auto matching_symbols =
      workspace.get_symbols_starting_with(tracked_file.m_game_version, symbol.value());
  lg::debug("get_completions - found {} symbols", matching_symbols.size());

  std::vector<LSPSpec::CompletionItem> items;
  items.reserve(matching_symbols.size());
  for (const auto& matching_symbol : matching_symbols) {
    LSPSpec::CompletionItem item;
    item.label = matching_symbol->m_name;
    item.kind = completion_item_kind_map.at(matching_symbol->m_kind);
    items.push_back(item);
  }

  LSPSpec::CompletionList list_result;
  list_result.isIncomplete = false;
  list_result.items = std::move(items);
  return list_result;
}

}  // namespace lsp_handlers
