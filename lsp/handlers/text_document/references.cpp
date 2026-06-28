#include "references.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/util/ast_util.h"
#include "lsp/lsp_util.h"

namespace {

bool is_list_like(TSNode node) {
  const std::string type = ts_node_type(node);
  return type == "list" || type == "form" || type == "list_lit" || type == "form_lit";
}

bool is_symbol_like(TSNode node) {
  const std::string type = ts_node_type(node);
  return type == "sym_name" || type == "sym_lit";
}

TSNode symbol_name_node(TSNode node) {
  if (ts_node_is_null(node)) {
    return node;
  }
  if (std::string(ts_node_type(node)) == "sym_name") {
    return node;
  }
  if (std::string(ts_node_type(node)) == "sym_lit") {
    for (uint32_t i = 0; i < ts_node_child_count(node); i++) {
      TSNode child = ts_node_child(node, i);
      if (std::string(ts_node_type(child)) == "sym_name") {
        return child;
      }
    }
  }
  return node;
}

std::string node_text(const WorkspaceOGFile& file, TSNode node) {
  return ast_util::get_source_code(file.m_content, symbol_name_node(node));
}

LSPSpec::Range node_range(TSNode node) {
  node = symbol_name_node(node);
  const auto start = ts_node_start_point(node);
  const auto end = ts_node_end_point(node);
  return LSPSpec::Range{{start.row, start.column}, {end.row, end.column}};
}

LSPSpec::Location make_location(const LSPSpec::DocumentUri& uri, TSNode node) {
  LSPSpec::Location location;
  location.m_uri = uri;
  location.m_range = node_range(node);
  return location;
}

LSPSpec::Location make_location(const LSPSpec::DocumentUri& uri,
                                const symbol_info::DefinitionLocation& def_loc,
                                const std::string& token) {
  LSPSpec::Location location;
  location.m_uri = uri;
  location.m_range.m_start = {def_loc.line_idx, def_loc.char_idx};
  location.m_range.m_end = {def_loc.line_idx, def_loc.char_idx + (uint32_t)token.length()};
  return location;
}

LSPSpec::Location make_location(const symbol_info::ReferenceLocation& ref,
                                const std::string& token) {
  LSPSpec::Location location;
  location.m_uri = lsp_util::uri_from_path(ref.file_path);
  location.m_range.m_start = {ref.line_idx, ref.char_idx};
  location.m_range.m_end = {ref.line_idx, ref.char_idx + (uint32_t)token.length()};
  return location;
}

std::string location_key(const LSPSpec::Location& location) {
  return location.m_uri + ":" + std::to_string(location.m_range.m_start.m_line) + ":" +
         std::to_string(location.m_range.m_start.m_character) + ":" +
         std::to_string(location.m_range.m_end.m_line) + ":" +
         std::to_string(location.m_range.m_end.m_character);
}

void add_unique(std::vector<LSPSpec::Location>& locations,
                std::set<std::string>& seen,
                const LSPSpec::Location& location) {
  const auto key = location_key(location);
  if (seen.insert(key).second) {
    locations.push_back(location);
  }
}

bool same_node_range(TSNode lhs, TSNode rhs) {
  lhs = symbol_name_node(lhs);
  rhs = symbol_name_node(rhs);
  return ts_node_start_byte(lhs) == ts_node_start_byte(rhs) &&
         ts_node_end_byte(lhs) == ts_node_end_byte(rhs);
}

void collect_symbol_nodes(const WorkspaceOGFile& file,
                          TSNode node,
                          const std::string& symbol_name,
                          std::vector<TSNode>& out) {
  if (ts_node_is_null(node)) {
    return;
  }
  if (std::string(ts_node_type(node)) == "sym_name" && node_text(file, node) == symbol_name) {
    out.push_back(node);
  }
  for (uint32_t i = 0; i < ts_node_child_count(node); i++) {
    collect_symbol_nodes(file, ts_node_child(node, i), symbol_name, out);
  }
}

std::vector<TSNode> collect_symbol_nodes(const WorkspaceOGFile& file,
                                         const std::string& symbol_name) {
  std::vector<TSNode> nodes;
  if (file.get_ast()) {
    collect_symbol_nodes(file, ts_tree_root_node(file.get_ast().get()), symbol_name, nodes);
  }
  return nodes;
}

TSNode enclosing_top_level(TSNode node) {
  TSNode result = {{0, 0, 0, 0}};
  TSNode curr = node;
  while (!ts_node_is_null(curr)) {
    if (is_list_like(curr)) {
      result = curr;
    }
    curr = ts_node_parent(curr);
  }
  return result;
}

std::string form_head(const WorkspaceOGFile& file, TSNode form) {
  if (!is_list_like(form) || ts_node_named_child_count(form) == 0) {
    return "";
  }
  return ast_util::get_source_code(file.m_content, ts_node_named_child(form, 0));
}

bool is_symbol_in_form_head(const WorkspaceOGFile& file, TSNode node, const std::string& head) {
  TSNode parent = ts_node_parent(node);
  while (!ts_node_is_null(parent) && !is_list_like(parent)) {
    parent = ts_node_parent(parent);
  }
  return form_head(file, parent) == head;
}

bool is_defining_symbol(const WorkspaceOGFile& file, TSNode node) {
  TSNode form = enclosing_top_level(node);
  const auto head = form_head(file, form);
  if (head == "define" || head == "defglobal" || head == "define-perm" ||
      head == "define-extern" || head == "defun" || head == "defmacro" ||
      head == "deftype" || head == "defbehavior") {
    if (ts_node_named_child_count(form) > 1) {
      return same_node_range(node, ts_node_named_child(form, 1));
    }
  }
  return false;
}

std::optional<TSNode> preferred_declaration(const WorkspaceOGFile& file,
                                            const std::string& symbol_name) {
  std::optional<TSNode> extern_decl;
  const auto nodes = collect_symbol_nodes(file, symbol_name);
  for (const auto& node : nodes) {
    if (!is_defining_symbol(file, node)) {
      continue;
    }
    const auto head = form_head(file, enclosing_top_level(node));
    if (head == "define-extern") {
      extern_decl = node;
    } else {
      return node;
    }
  }
  return extern_decl;
}

struct FieldIdentity {
  std::string owner_type;
  std::string field_name;
  TSNode field_node;
  bool is_declaration = false;
};

std::optional<std::string> field_decl_owner_type(const WorkspaceOGFile& file, TSNode node) {
  TSNode curr = ts_node_parent(node);
  while (!ts_node_is_null(curr)) {
    if (is_list_like(curr) && form_head(file, curr) == "deftype" &&
        ts_node_named_child_count(curr) > 1) {
      return ast_util::get_source_code(file.m_content, ts_node_named_child(curr, 1));
    }
    curr = ts_node_parent(curr);
  }
  return {};
}

std::vector<TSNode> symbol_children(TSNode form) {
  std::vector<TSNode> result;
  for (uint32_t i = 0; i < ts_node_child_count(form); i++) {
    TSNode child = ts_node_child(form, i);
    if (is_symbol_like(child)) {
      result.push_back(child);
    }
  }
  return result;
}

std::optional<FieldIdentity> resolve_arrow_field(const WorkspaceOGFile& file,
                                                 Workspace& workspace,
                                                 TSNode query_node) {
  TSNode normalized_query = symbol_name_node(query_node);
  TSNode curr = normalized_query;
  int depth = 0;
  while (!ts_node_is_null(curr) && depth < 4) {
    if (is_list_like(curr) && form_head(file, curr) == "->") {
      auto syms = symbol_children(curr);
      int query_index = -1;
      for (size_t i = 0; i < syms.size(); i++) {
        if (same_node_range(syms.at(i), normalized_query)) {
          query_index = (int)i;
          break;
        }
      }
      if (query_index < 2) {
        return {};
      }

      std::string owner_type = infer_type(file, syms.at(1), workspace);
      if (owner_type.empty()) {
        auto type_info = workspace.get_symbol_typeinfo(file, node_text(file, syms.at(1)));
        if (type_info) {
          owner_type = type_info->first.base_type();
        }
      }
      for (int i = 2; !owner_type.empty() && i < query_index; i++) {
        auto field = workspace.get_field_info(file, owner_type, node_text(file, syms.at(i)));
        owner_type = field ? field->type : "";
      }
      if (owner_type.empty()) {
        return {};
      }
      return FieldIdentity{owner_type, node_text(file, normalized_query), normalized_query, false};
    }
    curr = ts_node_parent(curr);
    depth++;
  }
  return {};
}

std::optional<FieldIdentity> resolve_field_identity(const WorkspaceOGFile& file,
                                                    Workspace& workspace,
                                                    TSNode query_node) {
  if (auto arrow = resolve_arrow_field(file, workspace, query_node)) {
    return arrow;
  }
  if (auto owner_type = field_decl_owner_type(file, query_node)) {
    if (workspace.get_field_info(file, *owner_type, node_text(file, query_node))) {
      return FieldIdentity{*owner_type, node_text(file, query_node), symbol_name_node(query_node),
                           true};
    }
  }
  return {};
}

std::vector<LSPSpec::Location> lexical_references(Workspace& workspace,
                                                  const WorkspaceOGFile& file,
                                                  const LSPSpec::DocumentUri& uri,
                                                  TSNode query_node,
                                                  bool include_declaration) {
  std::vector<LSPSpec::Location> locations;
  std::set<std::string> seen;
  if (resolve_arrow_field(file, workspace, query_node)) {
    return locations;
  }

  auto binding = find_lexical_binding(file, query_node, workspace);
  if (!binding) {
    for (const auto& candidate : collect_symbol_nodes(file, node_text(file, query_node))) {
      auto candidate_binding = find_lexical_binding(file, candidate, workspace);
      if (candidate_binding && same_node_range(candidate_binding->decl_node, query_node)) {
        binding = candidate_binding;
        break;
      }
    }
  }
  if (!binding) {
    return locations;
  }

  const auto candidates = collect_symbol_nodes(file, binding->name);
  for (const auto& candidate : candidates) {
    if (resolve_arrow_field(file, workspace, candidate)) {
      continue;
    }
    const bool is_decl = same_node_range(candidate, binding->decl_node);
    auto candidate_binding = find_lexical_binding(file, candidate, workspace);
    if (!candidate_binding && !is_decl) {
      continue;
    }
    if (candidate_binding && !same_node_range(candidate_binding->decl_node, binding->decl_node)) {
      continue;
    }
    if (!include_declaration && is_decl) {
      continue;
    }
    add_unique(locations, seen, make_location(uri, candidate));
  }
  return locations;
}

std::vector<LSPSpec::Location> field_references(Workspace& workspace,
                                                const WorkspaceOGFile& file,
                                                const LSPSpec::DocumentUri& uri,
                                                TSNode query_node,
                                                bool include_declaration) {
  std::vector<LSPSpec::Location> locations;
  std::set<std::string> seen;
  const auto identity = resolve_field_identity(file, workspace, query_node);
  if (!identity) {
    return locations;
  }

  const auto candidates = collect_symbol_nodes(file, identity->field_name);
  for (const auto& candidate : candidates) {
    if (auto candidate_identity = resolve_field_identity(file, workspace, candidate)) {
      if (candidate_identity->owner_type != identity->owner_type ||
          candidate_identity->field_name != identity->field_name) {
        continue;
      }
      if (!include_declaration && candidate_identity->is_declaration) {
        continue;
      }
      add_unique(locations, seen, make_location(uri, candidate));
    }
  }
  return locations;
}

std::vector<LSPSpec::Location> current_file_symbol_references(Workspace& workspace,
                                                              const WorkspaceOGFile& file,
                                                              const LSPSpec::DocumentUri& uri,
                                                              const std::string& symbol_name,
                                                              bool include_declaration) {
  (void)workspace;
  std::vector<LSPSpec::Location> locations;
  std::set<std::string> seen;
  const auto declaration = preferred_declaration(file, symbol_name);
  const auto declaration_form =
      declaration ? std::optional<TSNode>{enclosing_top_level(*declaration)} : std::nullopt;

  for (const auto& candidate : collect_symbol_nodes(file, symbol_name)) {
    if (is_under_arrow_field_pos(candidate, file)) {
      continue;
    }
    if (find_lexical_binding(file, candidate, workspace)) {
      continue;
    }

    const bool is_decl = declaration && same_node_range(candidate, *declaration);
    if (is_defining_symbol(file, candidate) && !is_decl) {
      continue;
    }
    if (!include_declaration && is_decl) {
      continue;
    }
    if (declaration_form && form_head(file, *declaration_form) == "defbehavior" &&
        is_symbol_in_form_head(file, candidate, "define-extern")) {
      continue;
    }
    add_unique(locations, seen, make_location(uri, candidate));
  }
  return locations;
}

void sort_locations(std::vector<LSPSpec::Location>& locations) {
  std::sort(locations.begin(), locations.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.m_uri != rhs.m_uri) {
      return lhs.m_uri < rhs.m_uri;
    }
    if (lhs.m_range.m_start.m_line != rhs.m_range.m_start.m_line) {
      return lhs.m_range.m_start.m_line < rhs.m_range.m_start.m_line;
    }
    return lhs.m_range.m_start.m_character < rhs.m_range.m_start.m_character;
  });
}

}  // namespace

namespace lsp_handlers {

std::optional<json> references(Workspace& workspace, json /*id*/, json raw_params) {
  auto params = raw_params.get<LSPSpec::ReferenceParams>();
  workspace.ensure_file_tracked(params.m_textDocument.m_uri);
  const auto maybe_file = workspace.get_tracked_og_file(params.m_textDocument.m_uri);
  if (!maybe_file) {
    return std::vector<LSPSpec::Location>{};
  }

  const auto& file = maybe_file->get();
  TSNode query_node = symbol_name_node(file.get_node_at_position(params.m_position));
  if (ts_node_is_null(query_node) || std::string(ts_node_type(query_node)) != "sym_name") {
    return std::vector<LSPSpec::Location>{};
  }

  const auto symbol_name = node_text(file, query_node);
  if (symbol_name.empty()) {
    return std::vector<LSPSpec::Location>{};
  }

  std::vector<LSPSpec::Location> locations;
  std::set<std::string> seen;
  const auto uri = Workspace::normalize_uri(params.m_textDocument.m_uri);

  for (const auto& location :
       lexical_references(workspace, file, uri, query_node, params.m_context.m_includeDeclaration)) {
    add_unique(locations, seen, location);
  }
  if (!locations.empty()) {
    sort_locations(locations);
    return locations;
  }

  for (const auto& location :
       field_references(workspace, file, uri, query_node, params.m_context.m_includeDeclaration)) {
    add_unique(locations, seen, location);
  }
  if (!locations.empty()) {
    sort_locations(locations);
    return locations;
  }

  for (const auto& location : current_file_symbol_references(
           workspace, file, uri, symbol_name, params.m_context.m_includeDeclaration)) {
    add_unique(locations, seen, location);
  }

  const auto compiler_refs = workspace.get_symbol_references(file, symbol_name);
  if (params.m_context.m_includeDeclaration) {
    const auto symbol_info = workspace.get_global_symbol_info(file, symbol_name);
    if (symbol_info && symbol_info.value()->m_def_location) {
      const auto& def_loc = *symbol_info.value()->m_def_location;
      auto def_uri = lsp_util::uri_from_path(def_loc.file_path);
      if (def_uri != "file:///Program%20string") {
        add_unique(locations, seen, make_location(def_uri, def_loc, symbol_name));
      }
    }
  }

  for (const auto& ref : compiler_refs) {
    auto location = make_location(ref, symbol_name);
    if (location.m_uri == "file:///Program%20string") {
      continue;
    }
    add_unique(locations, seen, location);
  }

  sort_locations(locations);
  return locations;
}

}  // namespace lsp_handlers
