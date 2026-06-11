#include "hover.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "common/util/ast_util.h"
#include "lsp/protocol/hover_formatting.h"

namespace {

TSNode normalize_symbol_node(TSNode node) {
  if (!ts_node_is_null(node) && std::string(ts_node_type(node)) == "sym_name") {
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent) && std::string(ts_node_type(parent)) == "sym_lit") {
      return parent;
    }
  }
  return node;
}

std::string normalized_type_or_unknown(const std::string& type) {
  return type.empty() ? "unknown" : type;
}

std::vector<lsp_hover::HoverParam> to_hover_params(const std::vector<Docs::ArgumentDocumentation>& args) {
  std::vector<lsp_hover::HoverParam> params;
  params.reserve(args.size());
  for (const auto& arg : args) {
    params.push_back({.name = arg.name, .type = arg.type, .doc = arg.description});
  }
  return params;
}

std::vector<lsp_hover::HoverParam> to_hover_fields(const std::vector<symbol_info::FieldInfo>& fields) {
  std::vector<lsp_hover::HoverParam> hover_fields;
  hover_fields.reserve(fields.size());
  for (const auto& field : fields) {
    hover_fields.push_back({.name = field.name, .type = field.type, .doc = field.description});
  }
  return hover_fields;
}

lsp_hover::HoverKind hover_kind_from_symbol_kind(const symbol_info::Kind kind) {
  switch (kind) {
    case symbol_info::Kind::FUNCTION:
    case symbol_info::Kind::LANGUAGE_BUILTIN:
      return lsp_hover::HoverKind::Function;
    case symbol_info::Kind::METHOD:
      return lsp_hover::HoverKind::Method;
    case symbol_info::Kind::MACRO:
      return lsp_hover::HoverKind::Macro;
    case symbol_info::Kind::GLOBAL_VAR:
      return lsp_hover::HoverKind::Global;
    case symbol_info::Kind::CONSTANT:
      return lsp_hover::HoverKind::Constant;
    case symbol_info::Kind::TYPE:
      return lsp_hover::HoverKind::Type;
    case symbol_info::Kind::ENUM:
      return lsp_hover::HoverKind::EnumValue;
    case symbol_info::Kind::STATE:
    case symbol_info::Kind::FWD_DECLARED_SYM:
    case symbol_info::Kind::INVALID:
    default:
      return lsp_hover::HoverKind::Unknown;
  }
}

std::optional<LSPSpec::Hover> make_lexical_hover(const LexicalBinding& binding) {
  lsp_hover::HoverInfo info;
  info.kind = binding.kind == "parameter" ? lsp_hover::HoverKind::Parameter : lsp_hover::HoverKind::Local;
  if (binding.kind == "implicit self") {
    info.kind = lsp_hover::HoverKind::Local;
    info.docstring = "implicit self";
  }
  info.name = binding.name;
  info.type = normalized_type_or_unknown(binding.type);
  return lsp_hover::make_markdown_hover(info);
}

std::optional<LSPSpec::Hover> make_field_hover(Workspace& workspace,
                                               const WorkspaceOGFile& tracked_file,
                                               const std::string& parent_type_name,
                                               const std::string& field_name) {
  auto field_info = workspace.get_field_info(tracked_file, parent_type_name, field_name);
  if (!field_info) {
    return {};
  }

  lsp_hover::HoverInfo info;
  info.kind = lsp_hover::HoverKind::Field;
  info.name = field_info->name;
  info.type = field_info->type;
  info.owner_type = parent_type_name;
  info.docstring = field_info->description;
  return lsp_hover::make_markdown_hover(info);
}

std::optional<LSPSpec::Hover> make_symbol_hover(Workspace& workspace,
                                                const WorkspaceOGFile& tracked_file,
                                                const std::string& symbol_name,
                                                const symbol_info::SymbolInfo& symbol_info) {
  lsp_hover::HoverInfo info;
  info.kind = hover_kind_from_symbol_kind(symbol_info.m_kind);
  info.name = symbol_name;
  info.docstring = symbol_info.m_docstring;

  switch (symbol_info.m_kind) {
    case symbol_info::Kind::GLOBAL_VAR:
    case symbol_info::Kind::CONSTANT:
      info.type = normalized_type_or_unknown(symbol_info.m_type);
      break;
    case symbol_info::Kind::FUNCTION:
    case symbol_info::Kind::LANGUAGE_BUILTIN:
      info.return_type = normalized_type_or_unknown(symbol_info.m_return_type);
      info.params = to_hover_params(Docs::get_args_from_docstring(symbol_info.m_args,
                                                                  symbol_info.m_docstring));
      break;
    case symbol_info::Kind::METHOD:
      info.owner_type = symbol_info.m_method_info.type_name;
      info.return_type = symbol_info.m_method_info.type.arg_count() > 0
                             ? symbol_info.m_method_info.type.last_arg().base_type()
                             : "";
      info.params = to_hover_params(Docs::get_args_from_docstring(symbol_info.m_args,
                                                                  symbol_info.m_docstring));
      break;
    case symbol_info::Kind::MACRO:
      break;
    case symbol_info::Kind::TYPE: {
      info.parent_type = symbol_info.m_parent_type;
      info.fields = to_hover_fields(symbol_info.m_type_fields);
      break;
    }
    case symbol_info::Kind::ENUM:
      info.type = symbol_info.m_enum_info ? symbol_info.m_enum_info->print() : "";
      break;
    case symbol_info::Kind::FWD_DECLARED_SYM: {
      auto type_info = workspace.get_symbol_typeinfo(tracked_file, symbol_name);
      if (!type_info) {
        return {};
      }
      if (type_info->first.base_type() == "function") {
        info.kind = lsp_hover::HoverKind::Function;
        info.return_type = normalized_type_or_unknown(type_info->first.last_arg().base_type());
      } else {
        info.kind = lsp_hover::HoverKind::Global;
        info.type = normalized_type_or_unknown(type_info->first.base_type());
      }
      break;
    }
    case symbol_info::Kind::STATE:
    case symbol_info::Kind::INVALID:
    default:
      break;
  }

  if (info.kind == lsp_hover::HoverKind::Unknown) {
    return {};
  }

  return lsp_hover::make_markdown_hover(info);
}

std::optional<LSPSpec::Hover> hover_from_arrow_field(Workspace& workspace,
                                                     const WorkspaceOGFile& tracked_file,
                                                     TSNode node) {
  TSNode curr = node;
  int depth = 0;
  while (!ts_node_is_null(curr) && depth < 3) {
    std::string curr_type = ts_node_type(curr);
    if (curr_type == "list" || curr_type == "form" || curr_type == "list_lit" ||
        curr_type == "form_lit") {
      TSNode first_symbol = {{0, 0, 0, 0}};
      const uint32_t search_limit = std::min(ts_node_child_count(curr), (uint32_t)3);
      for (uint32_t i = 0; i < search_limit; i++) {
        TSNode child = ts_node_child(curr, i);
        std::string c_type = ts_node_type(child);
        if (c_type == "sym_name" || c_type == "sym_lit") {
          first_symbol = child;
          break;
        }
      }

      if (!ts_node_is_null(first_symbol) &&
          ast_util::get_source_code(tracked_file.m_content, first_symbol) == "->") {
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

        if (my_sym_idx >= 2) {
          std::string parent_type_name;
          TSNode obj_node = sym_nodes[1];
          std::string obj_name = ast_util::get_source_code(tracked_file.m_content, obj_node);
          parent_type_name = infer_type(tracked_file, obj_node, workspace);
          if (parent_type_name.empty()) {
            auto type_info = workspace.get_symbol_typeinfo(tracked_file, obj_name);
            if (type_info) {
              parent_type_name = type_info->first.base_type();
            }
          }

          if (!parent_type_name.empty()) {
            for (int i = 2; i < my_sym_idx; i++) {
              std::string step_name = ast_util::get_source_code(tracked_file.m_content, sym_nodes[i]);
              auto step_field = workspace.get_field_info(tracked_file, parent_type_name, step_name);
              if (step_field) {
                parent_type_name = step_field->type;
              } else {
                parent_type_name.clear();
                break;
              }
            }
          }

          if (!parent_type_name.empty()) {
            std::string field_name = ast_util::get_source_code(tracked_file.m_content, node);
            if (std::string(ts_node_type(node)) == "sym_lit" && ts_node_child_count(node) > 0) {
              for (uint32_t k = 0; k < ts_node_child_count(node); k++) {
                if (std::string(ts_node_type(ts_node_child(node, k))) == "sym_name") {
                  field_name = ast_util::get_source_code(tracked_file.m_content, ts_node_child(node, k));
                  break;
                }
              }
            }
            return make_field_hover(workspace, tracked_file, parent_type_name, field_name);
          }
        }
      }
    }
    curr = ts_node_parent(curr);
    depth++;
  }

  return {};
}

}  // namespace

std::optional<LSPSpec::Hover> hover_handler_ir(Workspace& /*workspace*/,
                                               const LSPSpec::TextDocumentPositionParams& /*params*/,
                                               const WorkspaceIRFile& /*tracked_file*/) {
  return {};
}

namespace lsp_handlers {
std::optional<json> hover(Workspace& workspace, json /*id*/, json raw_params) {
  auto params = raw_params.get<LSPSpec::TextDocumentPositionParams>();
  workspace.ensure_file_tracked(params.m_textDocument.m_uri);
  auto file_type = workspace.determine_filetype_from_uri(params.m_textDocument.m_uri);

  if (file_type == Workspace::FileType::OpenGOALIR) {
    auto tracked_file = workspace.get_tracked_ir_file(params.m_textDocument.m_uri);
    if (!tracked_file) {
      return {};
    }
    auto hover = hover_handler_ir(workspace, params, tracked_file.value());
    if (hover) {
      return json(*hover);
    }
    return {};
  }

  if (file_type != Workspace::FileType::OpenGOAL) {
    return {};
  }

  auto maybe_tracked_file = workspace.get_tracked_og_file(params.m_textDocument.m_uri);
  if (!maybe_tracked_file) {
    return {};
  }
  const auto& tracked_file = maybe_tracked_file.value().get();

  const auto symbol = tracked_file.get_symbol_at_position(params.m_position);
  TSNode node = normalize_symbol_node(tracked_file.get_node_at_position(params.m_position));

  if (!is_under_arrow_field_pos(node, tracked_file)) {
    auto lex_binding = find_lexical_binding(tracked_file, node, workspace);
    if (lex_binding) {
      auto hover = make_lexical_hover(*lex_binding);
      if (hover) {
        return json(*hover);
      }
      return {};
    }
  }

  auto field_hover = hover_from_arrow_field(workspace, tracked_file, node);
  if (field_hover) {
    return json(*field_hover);
  }

  if (!symbol) {
    return {};
  }

  const auto& symbol_info = workspace.get_global_symbol_info(tracked_file, symbol.value());
  if (!symbol_info) {
    return {};
  }

  auto hover = make_symbol_hover(workspace, tracked_file, symbol.value(), **symbol_info);
  if (hover) {
    return json(*hover);
  }
  return {};
}
}  // namespace lsp_handlers
