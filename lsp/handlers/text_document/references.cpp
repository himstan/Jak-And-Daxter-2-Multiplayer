#include "references.h"
#include "lsp/lsp_util.h"

namespace lsp_handlers {
std::optional<json> references(Workspace& workspace, json /*id*/, json raw_params) {
  auto params = raw_params.get<LSPSpec::ReferenceParams>();
  workspace.ensure_file_tracked(params.m_textDocument.m_uri);
  const auto file = workspace.get_tracked_og_file(params.m_textDocument.m_uri);
  if (!file) {
    return std::vector<LSPSpec::Location>{};
  }

  const auto symbol_name = file->get().get_symbol_at_position(params.m_position);
  if (!symbol_name) {
    return std::vector<LSPSpec::Location>{};
  }

  const auto references = workspace.get_symbol_references(file->get(), symbol_name.value());
  std::vector<LSPSpec::Location> locations;

  if (params.m_context.m_includeDeclaration) {
    const auto symbol_info = workspace.get_global_symbol_info(file->get(), symbol_name.value());
    if (symbol_info && symbol_info.value()->m_def_location) {
      const auto& def_loc = symbol_info.value()->m_def_location;
      LSPSpec::Location location;
      location.m_uri = lsp_util::uri_from_path(def_loc->file_path);
      location.m_range.m_start = {(uint32_t)def_loc->line_idx, (uint32_t)def_loc->char_idx};
      location.m_range.m_end = {(uint32_t)def_loc->line_idx, (uint32_t)def_loc->char_idx};
      locations.push_back(location);
    }
  }

  for (const auto& ref : references) {
    LSPSpec::Location location;
    location.m_uri = lsp_util::uri_from_path(ref.file_path);
    location.m_range.m_start = {(uint32_t)ref.line_idx, (uint32_t)ref.char_idx};
    location.m_range.m_end = {(uint32_t)ref.line_idx, (uint32_t)ref.char_idx};
    locations.push_back(location);
  }

  return locations;
}
}  // namespace lsp_handlers
