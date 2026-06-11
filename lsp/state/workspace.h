#pragma once

#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>

#include "common/util/FileUtil.h"

#include "decompiler/util/DecompilerTypeSystem.h"
#include "goalc/compiler/Compiler.h"
#include "goalc/compiler/docs/DocTypes.h"
#include "lsp/protocol/common_types.h"
#include "lsp/protocol/document_diagnostics.h"
#include "lsp/protocol/document_symbols.h"
#include "lsp/state/lsp_requester.h"

#include "third-party/tree-sitter/tree-sitter/lib/src/tree.h"

// TODO -
// https://sourcegraph.com/github.com/ensisoft/detonator@36f626caf957d0734865a8f5641be6170d997f45/-/blob/editor/app/lua-tools.cpp?L116:15-116:30

struct TreeSitterTreeDeleter {
  void operator()(TSTree* ptr) const { ts_tree_delete(ptr); }
};

struct OpenGOALFormResult {
  std::vector<std::string> tokens;
  std::pair<int, int> start_point;
  std::pair<int, int> end_point;
};

struct OGGlobalIndex {
  std::unordered_map<std::string, Docs::SymbolDocumentation> global_symbols = {};
  std::unordered_map<std::string, Docs::FileDocumentation> per_file_symbols = {};
};

struct LexicalBinding {
  std::string name;
  std::string kind; // "parameter", "local"
  std::string type; // e.g. "mp-battle-event", empty if unknown
  TSNode decl_node;
};

class Workspace;
class WorkspaceOGFile;
std::optional<LexicalBinding> find_lexical_binding(const WorkspaceOGFile& file, TSNode query_node, Workspace& workspace);
std::string infer_type(const WorkspaceOGFile& file, TSNode node, Workspace& workspace);
bool is_under_arrow_field_pos(TSNode node, const WorkspaceOGFile& file);

class WorkspaceOGFile {
 public:
  WorkspaceOGFile(){};
  WorkspaceOGFile(const LSPSpec::DocumentUri& uri,
                  const std::string& content,
                  const GameVersion& game_version);
  LSPSpec::DocumentUri m_uri;
  std::string m_content;
  int m_line_count = 0;
  std::string m_line_ending;
  GameVersion m_game_version;
  std::vector<LSPSpec::DocumentSymbol> m_symbols;
  std::vector<LSPSpec::Diagnostic> m_diagnostics;

  void parse_content(const std::string& new_content);
  void update_symbols(const std::vector<symbol_info::SymbolInfo*>& symbol_infos);
  std::optional<std::string> get_symbol_at_position(const LSPSpec::Position position) const;
  TSNode get_node_at_position(const LSPSpec::Position position) const;
  std::vector<OpenGOALFormResult> search_for_forms_that_begin_with(
      std::vector<std::string> prefix) const;
  const std::shared_ptr<TSTree>& get_ast() const { return m_ast; }

 private:
  int32_t version;
  std::shared_ptr<TSTree> m_ast;
};

class WorkspaceIRFile {
 public:
  WorkspaceIRFile(){};
  WorkspaceIRFile(const std::string& content);
  // TODO - make private
  int32_t version;
  std::vector<std::string> m_lines;
  std::vector<LSPSpec::DocumentSymbol> m_symbols;
  std::vector<LSPSpec::Diagnostic> m_diagnostics;
  GameVersion m_game_version;
  LSPSpec::DocumentUri m_all_types_uri = "";
  fs::path m_all_types_file_path;

  std::optional<std::string> get_mips_instruction_at_position(
      const LSPSpec::Position position) const;
  std::optional<std::string> get_symbol_at_position(const LSPSpec::Position position) const;

 private:
  void find_all_types_path(const std::string& line);
  void find_function_symbol(const uint32_t line_num_zero_based, const std::string& line);
  /// Make any relevant diagnostics on the IR line.
  /// It's assumed each line in an IR can have atmost one diagnostic, and they are contained to just
  /// that line!
  void identify_diagnostics(const uint32_t line_num_zero_based,
                            const std::string& line,
                            const bool in_opengoal_block);
};

class Workspace {
 public:
  enum class FileType { OpenGOAL, OpenGOALIR, Unsupported };
  Workspace();
  virtual ~Workspace();

  bool is_initialized();
  void set_initialized(bool new_value);

  // Even though when a file is initially opened it has the language id
  // many subsequent requests only provide a uri to the file
  // and it's a lot faster to check the end of a string, then multiple tracked file maps
  FileType determine_filetype_from_languageid(const std::string& language_id);
  FileType determine_filetype_from_uri(const LSPSpec::DocumentUri& file_uri);
  std::optional<GameVersion> determine_game_version_from_uri(const LSPSpec::DocumentUri& uri);

  void start_tracking_file(const LSPSpec::DocumentUri& file_uri,
                           const std::string& language_id,
                           const std::string& content);
  void ensure_file_tracked(const LSPSpec::DocumentUri& file_uri,
                           const std::optional<std::string>& language_id = {},
                           const std::optional<std::string>& content = {});
  void update_tracked_file(const LSPSpec::DocumentUri& file_uri, const std::string& content);
  void handle_file_save(const LSPSpec::DocumentUri& file_uri);
  void update_global_index(const GameVersion game_version);
  void stop_tracking_file(const LSPSpec::DocumentUri& file_uri);
  int track_file_count() const {
    return m_tracked_og_files.size() + m_tracked_ir_files.size();
  }
  std::optional<std::reference_wrapper<WorkspaceOGFile>> get_tracked_og_file(
      const LSPSpec::URI& file_uri);
  std::optional<std::reference_wrapper<WorkspaceIRFile>> get_tracked_ir_file(
      const LSPSpec::URI& file_uri);

  static LSPSpec::DocumentUri normalize_uri(const LSPSpec::DocumentUri& uri);
  std::vector<symbol_info::SymbolInfo*> get_symbols_starting_with(const GameVersion game_version,
                                                                  const std::string& symbol_prefix);
  std::optional<symbol_info::SymbolInfo*> get_global_symbol_info(const WorkspaceOGFile& file,
                                                                 const std::string& symbol_name);
  std::optional<std::pair<TypeSpec, Type*>> get_symbol_typeinfo(const WorkspaceOGFile& file,
                                                                const std::string& symbol_name);
  std::optional<symbol_info::DefinitionLocation> get_symbol_def_location(
      const WorkspaceOGFile& file,
      const symbol_info::SymbolInfo* symbol_info);
  std::vector<symbol_info::ReferenceLocation> get_symbol_references(
      const WorkspaceOGFile& file,
      const std::string& symbol_name);
  std::vector<symbol_info::FieldInfo> get_field_suggestions(const WorkspaceOGFile& file,
                                                            const std::string& type_name);
  std::optional<symbol_info::FieldInfo> get_field_info(const WorkspaceOGFile& file,
                                                      const std::string& type_name,
                                                      const std::string& field_name);
  std::vector<std::tuple<std::string, std::string, Docs::DefinitionLocation>>
  get_symbols_parent_type_path(const std::string& symbol_name, const GameVersion game_version);
  std::vector<std::tuple<std::string, std::string, Docs::DefinitionLocation>> get_types_subtypes(
      const std::string& symbol_name,
      const GameVersion game_version);
  std::unordered_map<std::string, s64> get_enum_entries(const std::string& enum_name,
                                                        const GameVersion game_version);
  bool is_compiler_ready(const GameVersion game_version) const;

  void set_client_capabilities(const json& caps) {
    m_client_capabilities = caps;
    if (caps.contains("window") && caps["window"].contains("workDoneProgress")) {
      m_work_done_progress_supported = caps["window"]["workDoneProgress"].get<bool>();
    }
  }
  bool work_done_progress_supported() const { return m_work_done_progress_supported; }

  void scan_file_for_defines(const WorkspaceOGFile& file);

  void inject_compiler(GameVersion version, std::unique_ptr<Compiler> compiler) {
    m_compiler_instances[version] = std::move(compiler);
  }
  Compiler* get_compiler(GameVersion version) {
    if (m_compiler_instances.find(version) != m_compiler_instances.end()) {
      return m_compiler_instances.at(version).get();
    }
    return nullptr;
  }
  void inject_tracked_og_file(const LSPSpec::DocumentUri& file_uri, WorkspaceOGFile file) {
    auto norm_uri = normalize_uri(file_uri);
    m_tracked_og_files[norm_uri] = std::move(file);
    scan_file_for_defines(m_tracked_og_files[norm_uri]);
  }

 private:
  LSPRequester m_requester;
  bool m_initialized = false;
  json m_client_capabilities;
  bool m_work_done_progress_supported = false;
  std::unordered_map<LSPSpec::DocumentUri, WorkspaceOGFile> m_tracked_og_files = {};
  std::unordered_map<LSPSpec::DocumentUri, WorkspaceIRFile> m_tracked_ir_files = {};

  // TODO:
  // OpenGOAL is still incredibly tightly coupled to the jak projects as a language
  //
  // In the future, information like GameVersion should just be within the project file
  // and then we can track projects instead of games
  //
  // Until that decoupling happens, things like this will remain fairly clunky.
  // TODO - change this to a shared_ptr so it can more easily be passed around functions
  std::unordered_map<GameVersion, std::unique_ptr<Compiler>> m_compiler_instances;
  std::unordered_map<GameVersion, OGGlobalIndex> m_global_indicies;
 public:
  std::unordered_map<GameVersion, std::unordered_map<std::string, std::string>> m_inferred_global_types;
};
