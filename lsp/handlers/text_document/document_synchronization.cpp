#include "document_synchronization.h"

namespace lsp_handlers {

void did_open(Workspace& workspace, json raw_params) {
  auto params = raw_params.get<LSPSpec::DidOpenTextDocumentParams>();
  workspace.ensure_file_tracked(params.m_textDocument.m_uri, params.m_textDocument.m_languageId,
                                params.m_textDocument.m_text);
}

void did_change(Workspace& workspace, json raw_params) {
  auto params = raw_params.get<LSPSpec::DidChangeTextDocumentParams>();
  for (const auto& change : params.m_contentChanges) {
    workspace.ensure_file_tracked(params.m_textDocument.m_uri, {}, change.m_text);
    workspace.update_tracked_file(params.m_textDocument.m_uri, change.m_text);
  }
}

void did_close(Workspace& workspace, json raw_params) {
  auto params = raw_params.get<LSPSpec::DidCloseTextDocumentParams>();
  workspace.stop_tracking_file(params.m_textDocument.m_uri);
}

void will_save(Workspace& workspace, json raw_params) {
  auto params = raw_params.get<LSPSpec::WillSaveTextDocumentParams>();
  workspace.ensure_file_tracked(params.textDocument.m_uri);
  workspace.handle_file_save(params.textDocument.m_uri);
}

void did_save(Workspace& workspace, json raw_params) {
  auto params = raw_params.get<LSPSpec::DidSaveTextDocumentParams>();
  workspace.ensure_file_tracked(params.textDocument.m_uri);
  workspace.handle_file_save(params.textDocument.m_uri);
}

std::optional<std::vector<json>> did_open_push_diagnostics(Workspace& workspace, json raw_params) {
  auto params = raw_params.get<LSPSpec::DidOpenTextDocumentParams>();
  workspace.ensure_file_tracked(params.m_textDocument.m_uri, params.m_textDocument.m_languageId,
                                params.m_textDocument.m_text);
  const auto file_type =
      workspace.determine_filetype_from_languageid(params.m_textDocument.m_languageId);

  LSPSpec::PublishDiagnosticParams publish_params;
  publish_params.m_uri = params.m_textDocument.m_uri;
  publish_params.m_version = params.m_textDocument.m_version;

  if (file_type == Workspace::FileType::OpenGOALIR) {
    auto maybe_tracked_file = workspace.get_tracked_ir_file(params.m_textDocument.m_uri);
    if (!maybe_tracked_file) {
      return {};
    }
    const auto& tracked_file = maybe_tracked_file.value().get();
    publish_params.m_diagnostics = tracked_file.m_diagnostics;
  } else if (file_type == Workspace::FileType::OpenGOAL) {
    auto maybe_tracked_file = workspace.get_tracked_og_file(params.m_textDocument.m_uri);
    if (!maybe_tracked_file) {
      return {};
    }
    const auto& tracked_file = maybe_tracked_file.value().get();
    publish_params.m_diagnostics = tracked_file.m_diagnostics;
  }

  for (const auto& diag : publish_params.m_diagnostics) {
    lg::info("publishDiagnostics uri={} count={} line={} char={}", publish_params.m_uri,
             publish_params.m_diagnostics.size(), diag.m_range.m_start.m_line,
             diag.m_range.m_start.m_character);
  }
  if (publish_params.m_diagnostics.empty()) {
    lg::info("publishDiagnostics uri={} count=0", publish_params.m_uri);
  }

  json response;
  response["jsonrpc"] = "2.0";
  response["method"] = "textDocument/publishDiagnostics";
  response["params"] = publish_params;

  return std::vector<json>{response};
}

std::optional<std::vector<json>> did_change_push_diagnostics(Workspace& workspace, json raw_params) {
  auto params = raw_params.get<LSPSpec::DidChangeTextDocumentParams>();
  workspace.ensure_file_tracked(params.m_textDocument.m_uri);
  const auto file_type = workspace.determine_filetype_from_uri(params.m_textDocument.m_uri);

  LSPSpec::PublishDiagnosticParams publish_params;
  publish_params.m_uri = params.m_textDocument.m_uri;
  publish_params.m_version = params.m_textDocument.m_version;

  if (file_type == Workspace::FileType::OpenGOALIR) {
    auto maybe_tracked_file = workspace.get_tracked_ir_file(params.m_textDocument.m_uri);
    if (!maybe_tracked_file) {
      return {};
    }
    const auto& tracked_file = maybe_tracked_file.value().get();
    publish_params.m_diagnostics = tracked_file.m_diagnostics;
  } else if (file_type == Workspace::FileType::OpenGOAL) {
    auto maybe_tracked_file = workspace.get_tracked_og_file(params.m_textDocument.m_uri);
    if (!maybe_tracked_file) {
      return {};
    }
    const auto& tracked_file = maybe_tracked_file.value().get();
    publish_params.m_diagnostics = tracked_file.m_diagnostics;
  }

  for (const auto& diag : publish_params.m_diagnostics) {
    lg::info("publishDiagnostics uri={} count={} line={} char={}", publish_params.m_uri,
             publish_params.m_diagnostics.size(), diag.m_range.m_start.m_line,
             diag.m_range.m_start.m_character);
  }
  if (publish_params.m_diagnostics.empty()) {
    lg::info("publishDiagnostics uri={} count=0", publish_params.m_uri);
  }

  json response;
  response["jsonrpc"] = "2.0";
  response["method"] = "textDocument/publishDiagnostics";
  response["params"] = publish_params;

  return std::vector<json>{response};
}

std::optional<std::vector<json>> did_save_push_diagnostics(Workspace& workspace, json raw_params) {
  auto params = raw_params.get<LSPSpec::DidSaveTextDocumentParams>();
  workspace.ensure_file_tracked(params.textDocument.m_uri);
  const auto file_type = workspace.determine_filetype_from_uri(params.textDocument.m_uri);

  LSPSpec::PublishDiagnosticParams publish_params;
  publish_params.m_uri = params.textDocument.m_uri;

  if (file_type == Workspace::FileType::OpenGOALIR) {
    auto maybe_tracked_file = workspace.get_tracked_ir_file(params.textDocument.m_uri);
    if (!maybe_tracked_file) {
      return {};
    }
    const auto& tracked_file = maybe_tracked_file.value().get();
    publish_params.m_diagnostics = tracked_file.m_diagnostics;
  } else if (file_type == Workspace::FileType::OpenGOAL) {
    auto maybe_tracked_file = workspace.get_tracked_og_file(params.textDocument.m_uri);
    if (!maybe_tracked_file) {
      return {};
    }
    const auto& tracked_file = maybe_tracked_file.value().get();
    publish_params.m_diagnostics = tracked_file.m_diagnostics;
  }

  for (const auto& diag : publish_params.m_diagnostics) {
    lg::info("publishDiagnostics uri={} count={} line={} char={}", publish_params.m_uri,
             publish_params.m_diagnostics.size(), diag.m_range.m_start.m_line,
             diag.m_range.m_start.m_character);
  }
  if (publish_params.m_diagnostics.empty()) {
    lg::info("publishDiagnostics uri={} count=0", publish_params.m_uri);
  }

  json response;
  response["jsonrpc"] = "2.0";
  response["method"] = "textDocument/publishDiagnostics";
  response["params"] = publish_params;

  return std::vector<json>{response};
}

}  // namespace lsp_handlers
