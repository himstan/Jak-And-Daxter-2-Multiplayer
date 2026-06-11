#include "go_to.h"

#include "common/util/ast_util.h"
#include "lsp/lsp_util.h"

namespace lsp_handlers {
std::optional<json> go_to_definition(Workspace& workspace, json /*id*/, json raw_params) {
  auto params = raw_params.get<LSPSpec::TextDocumentPositionParams>();
  workspace.ensure_file_tracked(params.m_textDocument.m_uri);
  const auto file_type = workspace.determine_filetype_from_uri(params.m_textDocument.m_uri);

  json locations = json::array();

  if (file_type == Workspace::FileType::OpenGOALIR) {
    return {};
  } else if (file_type == Workspace::FileType::OpenGOAL) {
    auto maybe_tracked_file = workspace.get_tracked_og_file(params.m_textDocument.m_uri);
    if (!maybe_tracked_file) {
      return {};
    }
    const auto& tracked_file = maybe_tracked_file.value().get();
    const auto symbol = tracked_file.get_symbol_at_position(params.m_position);

    // Check if we are on a field in a (-> ...) form
    TSNode node = tracked_file.get_node_at_position(params.m_position);
    
    // Normalize: if we are on a sym_name, move up to sym_lit if it exists
    if (!ts_node_is_null(node) && std::string(ts_node_type(node)) == "sym_name") {
      TSNode parent = ts_node_parent(node);
      if (!ts_node_is_null(parent) && std::string(ts_node_type(parent)) == "sym_lit") {
        node = parent;
      }
    }

    if (!is_under_arrow_field_pos(node, tracked_file)) {
      auto lex_binding = find_lexical_binding(tracked_file, node, workspace);
      if (lex_binding) {
        auto decl_point = ts_node_start_point(lex_binding->decl_node);
        LSPSpec::Location location;
        location.m_uri = params.m_textDocument.m_uri;
        location.m_range.m_start = {decl_point.row, decl_point.column};
        location.m_range.m_end = {decl_point.row, decl_point.column + (uint32_t)lex_binding->name.length()};
        locations.push_back(location);
        return locations;
      }
    }

    TSNode curr = node;
    int depth = 0;
    while (!ts_node_is_null(curr) && depth < 3) {
      std::string curr_type = ts_node_type(curr);
      lg::debug("go_to_definition - climbing depth {}: type={}", depth, curr_type);
      if (curr_type == "list" || curr_type == "form" || curr_type == "list_lit" || curr_type == "form_lit") {
        TSNode first_symbol = {{0, 0, 0, 0}};
        uint32_t search_limit = std::min(ts_node_child_count(curr), (uint32_t)3);
        for (uint32_t i = 0; i < search_limit; i++) {
          TSNode child = ts_node_child(curr, i);
          std::string c_type = ts_node_type(child);
          if (c_type == "sym_name" || c_type == "sym_lit") {
            first_symbol = child;
            break;
          }
        }

        if (!ts_node_is_null(first_symbol)) {
          std::string first_elt = ast_util::get_source_code(tracked_file.m_content, first_symbol);
          lg::debug("go_to_definition - first element in form: {}", first_elt);
          if (first_elt == "->") {
            lg::debug("go_to_definition - found -> operator");
            std::vector<TSNode> sym_nodes;
            int my_sym_idx = -1;
            for (uint32_t i = 0; i < ts_node_child_count(curr); i++) {
              TSNode child = ts_node_child(curr, i);
              std::string c_type = ts_node_type(child);
              if (c_type == "sym_name" || c_type == "sym_lit") {
                if (ts_node_eq(child, node)) {
                  my_sym_idx = (int)sym_nodes.size();
                }
                sym_nodes.push_back(child);
              }
            }
            lg::debug("go_to_definition - my symbol index in form: {}", my_sym_idx);

            if (my_sym_idx >= 2) { // 0 is ->, 1 is obj, 2+ are fields
              std::string parent_type_name;
              TSNode obj_node = sym_nodes[1];
              std::string obj_name = ast_util::get_source_code(tracked_file.m_content, obj_node);
              lg::debug("go_to_definition - resolving root object: {}", obj_name);
              parent_type_name = infer_type(tracked_file, obj_node, workspace);
              if (parent_type_name.empty()) {
                auto type_info = workspace.get_symbol_typeinfo(tracked_file, obj_name);
                if (type_info) {
                  parent_type_name = type_info->first.base_type();
                }
              }
              lg::debug("go_to_definition - root object type: {}", parent_type_name);

              if (!parent_type_name.empty() && my_sym_idx > 2) {
                for (int i = 2; i < my_sym_idx; i++) {
                  std::string step_name = ast_util::get_source_code(tracked_file.m_content, sym_nodes[i]);
                  lg::debug("go_to_definition - resolving step {}: {} in type {}", i-1, step_name, parent_type_name);
                  auto step_field = workspace.get_field_info(tracked_file, parent_type_name, step_name);
                  if (step_field) {
                    parent_type_name = step_field->type;
                    lg::debug("go_to_definition - step {} resolved to type: {}", i-1, parent_type_name);
                  } else {
                    lg::debug("go_to_definition - step {} FAILED", i-1);
                    parent_type_name = "";
                    break;
                  }
                }
              }

              if (!parent_type_name.empty()) {
                std::string field_name = ast_util::get_source_code(tracked_file.m_content, node);
                // If it was a sym_lit, we might need the sym_name source specifically
                if (std::string(ts_node_type(node)) == "sym_lit" && ts_node_child_count(node) > 0) {
                   for(uint32_t k=0; k < ts_node_child_count(node); k++) {
                      if(std::string(ts_node_type(ts_node_child(node, k))) == "sym_name") {
                         field_name = ast_util::get_source_code(tracked_file.m_content, ts_node_child(node, k));
                         break;
                      }
                   }
                }

                auto field_info = workspace.get_field_info(tracked_file, parent_type_name, field_name);
                if (field_info && field_info->m_def_location) {
                  LSPSpec::Location location;
                  location.m_uri = lsp_util::uri_from_path(field_info->m_def_location->file_path);
                  location.m_range.m_start = {(uint32_t)field_info->m_def_location->line_idx,
                                              (uint32_t)field_info->m_def_location->char_idx};
                  location.m_range.m_end = {(uint32_t)field_info->m_def_location->line_idx,
                                            (uint32_t)field_info->m_def_location->char_idx};
                  locations.push_back(location);
                  return locations;
                }
              }
            }
            break; // Found the -> list, don't look higher
          }
        }
      }
      curr = ts_node_parent(curr);
    }

    if (!symbol) {
      return {};
    }
    const auto& symbol_info = workspace.get_global_symbol_info(tracked_file, symbol.value());
    if (!symbol_info) {
      return {};
    }

    const auto& def_loc = workspace.get_symbol_def_location(tracked_file, symbol_info.value());
    if (!def_loc) {
      return {};
    }

    LSPSpec::Location location;
    location.m_uri = lsp_util::uri_from_path(def_loc->file_path);
    location.m_range.m_start = {(uint32_t)def_loc->line_idx, (uint32_t)def_loc->char_idx};
    location.m_range.m_end = {(uint32_t)def_loc->line_idx, (uint32_t)def_loc->char_idx};
    locations.push_back(location);
  }

  return locations;
}
}  // namespace lsp_handlers
