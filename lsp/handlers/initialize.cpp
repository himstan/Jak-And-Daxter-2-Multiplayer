#include "initialize.h"

namespace lsp_handlers {
std::optional<json> initialize(Workspace& workspace, json /*id*/, json params) {
  if (params.contains("capabilities")) {
    workspace.set_client_capabilities(params["capabilities"]);
  }

  json text_document_sync{
      {"openClose", true},
      {"change", 1},  // Full sync
      {"willSave", true},
      {"willSaveWaitUntil", false},
      {"save", {{"includeText", false}}},
  };

  json completion_provider{
      {"resolveProvider", false},
      {"triggerCharacters", {" "}},
  };

  json result{{"capabilities",
               {
                   {"textDocumentSync", text_document_sync},
                   {"hoverProvider", true},
                   {"completionProvider", completion_provider},
                   {"definitionProvider", true},
                   {"colorProvider", true},
                   {"referencesProvider", true},
                   {"documentSymbolProvider", true},
                   {"documentFormattingProvider", true},
                   {"typeHierarchyProvider", true},
               }},
              {"serverInfo", {{"name", "OpenGOAL LSP"}}}};
  return result;
}
}  // namespace lsp_handlers
