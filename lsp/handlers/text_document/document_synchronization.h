#pragma once

#include <optional>

#include "common/util/json_util.h"

#include "lsp/protocol/document_diagnostics.h"
#include "lsp/protocol/document_synchronization.h"
#include "lsp/state/workspace.h"

namespace lsp_handlers {

void did_open(Workspace& workspace, json raw_params);
void did_change(Workspace& workspace, json raw_params);
void did_close(Workspace& workspace, json raw_params);
void will_save(Workspace& workspace, json raw_params);
void did_save(Workspace& workspace, json raw_params);

std::optional<std::vector<json>> did_open_push_diagnostics(Workspace& workspace, json raw_params);
std::optional<std::vector<json>> did_change_push_diagnostics(Workspace& workspace,
                                                             json raw_params);
std::optional<std::vector<json>> did_save_push_diagnostics(Workspace& workspace, json raw_params);

}  // namespace lsp_handlers
