#pragma once

#include <optional>

#include "common/util/json_util.h"

#include "lsp/protocol/common_types.h"
#include "lsp/state/workspace.h"

namespace lsp_handlers {
std::optional<json> go_to_definition(Workspace& workspace, json id, json raw_params);
}
