#pragma once

#include "lsp/protocol/common_types.h"
#include "lsp/state/workspace.h"

namespace lsp_handlers {
std::optional<json> references(Workspace& workspace, json id, json raw_params);
}  // namespace lsp_handlers
