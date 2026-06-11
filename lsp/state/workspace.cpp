#include "workspace.h"

#include <regex>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/ast_util.h"
#include "common/util/string_util.h"

#include "lsp/lsp_util.h"
#include "lsp/protocol/common_types.h"
#include "tree_sitter/api.h"

// Declare the `tree_sitter_opengoal` function, which is
// implemented by the `tree-sitter-opengoal` library.
extern "C" {
extern const TSLanguage* tree_sitter_opengoal();
}

const TSLanguage* g_opengoalLang = tree_sitter_opengoal();

Workspace::Workspace(){};
Workspace::~Workspace(){};

bool Workspace::is_initialized() {
  return m_initialized;
};

void Workspace::set_initialized(bool new_value) {
  m_initialized = new_value;
}

Workspace::FileType Workspace::determine_filetype_from_languageid(const std::string& language_id) {
  if (language_id == "opengoal") {
    return FileType::OpenGOAL;
  } else if (language_id == "opengoal-ir") {
    return FileType::OpenGOALIR;
  }
  return FileType::Unsupported;
}

Workspace::FileType Workspace::determine_filetype_from_uri(const LSPSpec::DocumentUri& file_uri) {
  if (str_util::ends_with(file_uri, ".gc")) {
    return FileType::OpenGOAL;
  } else if (str_util::ends_with(file_uri, "ir2.asm")) {
    return FileType::OpenGOALIR;
  }
  return FileType::Unsupported;
}

LSPSpec::DocumentUri Workspace::normalize_uri(const LSPSpec::DocumentUri& uri) {
  auto path = lsp_util::uri_to_path(uri);
  // Normalize path separators and casing on Windows
  auto fs_path = fs::path(path);
#ifdef _WIN32
  // On Windows, normalize to lowercase drive letter and consistent separators
  std::string path_str = file_util::convert_to_unix_path_separators(fs_path.string());
  if (path_str.length() >= 2 && path_str[1] == ':') {
    path_str[0] = std::tolower(path_str[0]);
  }
  return lsp_util::uri_from_path(fs::path(path_str));
#else
  return lsp_util::uri_from_path(fs_path);
#endif
}

std::optional<std::reference_wrapper<WorkspaceOGFile>> Workspace::get_tracked_og_file(
    const LSPSpec::URI& file_uri) {
  auto norm_uri = normalize_uri(file_uri);
  auto it = m_tracked_og_files.find(norm_uri);
  if (it == m_tracked_og_files.end()) {
    lg::debug("get_tracked_og_file lookup miss - raw: {}, norm: {}", file_uri, norm_uri);
    for (const auto& [key, _] : m_tracked_og_files) {
      lg::debug("  known tracked og key: {}", key);
    }
    return std::nullopt;
  }
  return std::ref(it->second);
}

std::optional<std::reference_wrapper<WorkspaceIRFile>> Workspace::get_tracked_ir_file(
    const LSPSpec::URI& file_uri) {
  auto norm_uri = normalize_uri(file_uri);
  auto it = m_tracked_ir_files.find(norm_uri);
  if (it == m_tracked_ir_files.end()) {
    lg::debug("get_tracked_ir_file lookup miss - raw: {}, norm: {}", file_uri, norm_uri);
    for (const auto& [key, _] : m_tracked_ir_files) {
      lg::debug("  known tracked ir key: {}", key);
    }
    return std::nullopt;
  }
  return std::ref(it->second);
}

// TODO - a gross hack that should go away when the language isn't so tightly coupled to the jak
// games
//
// This is bad because jak 2 now uses some code from the jak1 folder, and also wouldn't be able to
// be determined (jak1 or jak2?) if we had a proper 'common' folder(s).
std::optional<GameVersion> Workspace::determine_game_version_from_uri(
    const LSPSpec::DocumentUri& uri) {
  const auto path = lsp_util::uri_to_path(uri);
  lg::debug("determine_game_version_from_uri - path: {}", path);
  if (str_util::contains(path, "goal_src/jak1") || str_util::contains(path, "test/lsp/fixtures/jak1")) {
    lg::debug("detected game version: jak1");
    return GameVersion::Jak1;
  } else if (str_util::contains(path, "goal_src/jak2")) {
    lg::debug("detected game version: jak2");
    return GameVersion::Jak2;
  } else if (str_util::contains(path, "goal_src/jak3")) {
    lg::debug("detected game version: jak3");
    return GameVersion::Jak3;
  }
  lg::warn("could not detect game version from path: {}", path);
  return {};
}

std::vector<symbol_info::SymbolInfo*> Workspace::get_symbols_starting_with(
    const GameVersion game_version,
    const std::string& symbol_prefix) {
  if (m_compiler_instances.find(game_version) == m_compiler_instances.end()) {
    lg::debug("Compiler not instantiated for game version - {}",
              version_to_game_name(game_version));
    return {};
  }
  const auto& compiler = m_compiler_instances[game_version].get();
  return compiler->lookup_symbol_info_by_prefix(symbol_prefix);
}

std::optional<symbol_info::SymbolInfo*> Workspace::get_global_symbol_info(
    const WorkspaceOGFile& file,
    const std::string& symbol_name) {
  if (m_compiler_instances.find(file.m_game_version) == m_compiler_instances.end()) {
    lg::debug("Compiler not instantiated for game version - {}",
              version_to_game_name(file.m_game_version));
    return {};
  }
  const auto& compiler = m_compiler_instances[file.m_game_version].get();
  const auto symbol_infos = compiler->lookup_exact_name_info(symbol_name);
  if (symbol_infos.empty()) {
    return {};
  } else if (symbol_infos.size() > 1) {
    lg::debug("Found symbol info, but found multiple infos - {}", symbol_infos.size());
    // 1. Prefer non-fwd-dec (real definitions like defbehavior)
    for (auto* info : symbol_infos) {
      if (info->m_kind != symbol_info::Kind::FWD_DECLARED_SYM) {
        return info;
      }
    }
    // 2. Prefer current file
    std::string current_file_path =
        file_util::convert_to_unix_path_separators(lsp_util::uri_to_path(file.m_uri));
    for (auto* info : symbol_infos) {
      if (info->m_def_location) {
        std::string def_path =
            file_util::convert_to_unix_path_separators(info->m_def_location->file_path);
        if (def_path == current_file_path) {
          return info;
        }
      }
    }
    return symbol_infos.at(0);
  }
  const auto& symbol = symbol_infos.at(0);
  return symbol;
}

// TODO - consolidate what is needed into `SymbolInfo`
std::string infer_global_define_type(const WorkspaceOGFile& file, const symbol_info::SymbolInfo* sym_info, Workspace& workspace);

std::optional<std::pair<TypeSpec, Type*>> Workspace::get_symbol_typeinfo(
    const WorkspaceOGFile& file,
    const std::string& symbol_name) {
  if (m_compiler_instances.find(file.m_game_version) == m_compiler_instances.end()) {
    lg::debug("Compiler not instantiated for game version - {}",
              version_to_game_name(file.m_game_version));
    return {};
  }
  const auto& compiler = m_compiler_instances[file.m_game_version].get();

  // 1. Explicit compiler type first
  auto typespec = compiler->lookup_typespec(symbol_name);
  bool has_explicit_type = (typespec && typespec->base_type() != "none" && typespec->base_type() != "object");

  if (has_explicit_type) {
    const auto full_type_info = compiler->type_system().lookup_type_no_throw(typespec->base_type());
    if (full_type_info != nullptr) {
      return std::make_pair(typespec.value(), full_type_info);
    }
  }

  // 2. Existing sym_info->m_type if valid
  const auto symbol_infos = compiler->lookup_exact_name_info(symbol_name);
  if (!symbol_infos.empty()) {
    auto* sym_info = symbol_infos.at(0);
    if (sym_info->m_kind == symbol_info::Kind::GLOBAL_VAR) {
      if (!sym_info->m_type.empty()) {
        auto ts = TypeSpec(sym_info->m_type);
        const auto full_type_info = compiler->type_system().lookup_type_no_throw(ts.base_type());
        if (full_type_info != nullptr) {
          return std::make_pair(ts, full_type_info);
        }
      }
    }
  }

  // 3. Inferred define type map fallback
  if (m_inferred_global_types.count(file.m_game_version) > 0) {
    const auto& inner_map = m_inferred_global_types.at(file.m_game_version);
    if (inner_map.count(symbol_name) > 0) {
      const auto& inferred_type = inner_map.at(symbol_name);
      if (!inferred_type.empty()) {
        auto ts = TypeSpec(inferred_type);
        const auto full_type_info = compiler->type_system().lookup_type_no_throw(ts.base_type());
        if (full_type_info != nullptr) {
          return std::make_pair(ts, full_type_info);
        }
      }
    }
  }

  // 4. Unknown (generic/fallback typespec lookup if it was none or object)
  if (typespec) {
    const auto full_type_info = compiler->type_system().lookup_type_no_throw(typespec->base_type());
    if (full_type_info != nullptr) {
      return std::make_pair(typespec.value(), full_type_info);
    }
  }

  return {};
}

std::optional<symbol_info::DefinitionLocation> Workspace::get_symbol_def_location(
    const WorkspaceOGFile& /*file*/,
    const symbol_info::SymbolInfo* symbol_info) {
  const auto& def_loc = symbol_info->m_def_location;
  if (!def_loc) {
    return {};
  }
  return def_loc;
}

std::vector<symbol_info::FieldInfo> Workspace::get_field_suggestions(
    const WorkspaceOGFile& file,
    const std::string& type_name) {
  if (m_compiler_instances.find(file.m_game_version) == m_compiler_instances.end()) {
    return {};
  }
  const auto& compiler = m_compiler_instances[file.m_game_version].get();
  const auto type_info = compiler->type_system().lookup_type_no_throw(type_name);
  if (!type_info) {
    return {};
  }

  std::vector<symbol_info::FieldInfo> suggestions;
  auto struct_type = dynamic_cast<StructureType*>(type_info);
  if (struct_type) {
    for (const auto& field : struct_type->fields()) {
      symbol_info::FieldInfo field_info;
      field_info.name = field.name();
      field_info.type = field.type().print();
      field_info.description = "field of " + type_name;
      field_info.is_array = field.is_array();
      field_info.is_dynamic = field.is_dynamic();
      field_info.is_inline = field.is_inline();

      // Try to find the source location for this field
      // We look at the type where the field was actually DEFINED
      const auto defining_type_info = compiler->type_system().lookup_type_no_throw(type_name);
      if (defining_type_info && defining_type_info->m_field_metadata.count(field.name())) {
        const auto& meta = defining_type_info->m_field_metadata.at(field.name());
        symbol_info::DefinitionLocation def_loc;
        def_loc.file_path =
            file_util::convert_to_unix_path_separators(meta.definition_info->filename);
        def_loc.line_idx = meta.definition_info->line_idx_to_display;
        def_loc.char_idx = meta.definition_info->pos_in_line;
        field_info.m_def_location = def_loc;
      } else {
        // If not in the current type, it might be in a parent.
        std::string current_search_type = type_name;
        int hierarchy_depth = 0;
        while (!current_search_type.empty() && current_search_type != "none" && hierarchy_depth < 32) {
          const auto t = compiler->type_system().lookup_type_no_throw(current_search_type);
          if (t && t->m_field_metadata.count(field.name())) {
            const auto& meta = t->m_field_metadata.at(field.name());
            symbol_info::DefinitionLocation def_loc;
            def_loc.file_path =
                file_util::convert_to_unix_path_separators(meta.definition_info->filename);
            def_loc.line_idx = meta.definition_info->line_idx_to_display;
            def_loc.char_idx = meta.definition_info->pos_in_line;
            field_info.m_def_location = def_loc;
            break;
          }
          if (t) {
            std::string next_type = t->get_parent();
            if (next_type == current_search_type) {
              break;
            }
            current_search_type = next_type;
          } else {
            break;
          }
          hierarchy_depth++;
        }
      }

      suggestions.push_back(field_info);
    }
  }

  auto bitfield_type = dynamic_cast<BitFieldType*>(type_info);
  if (bitfield_type) {
    // TODO - handle bitfield suggestions
  }

  return suggestions;
}

std::optional<symbol_info::FieldInfo> Workspace::get_field_info(const WorkspaceOGFile& file,
                                                              const std::string& type_name,
                                                              const std::string& field_name) {
  const auto fields = get_field_suggestions(file, type_name);
  for (const auto& field : fields) {
    if (field.name == field_name) {
      return field;
    }
  }
  return {};
}

std::vector<symbol_info::ReferenceLocation> Workspace::get_symbol_references(
    const WorkspaceOGFile& file,
    const std::string& symbol_name) {
  if (m_compiler_instances.find(file.m_game_version) != m_compiler_instances.end()) {
    return m_compiler_instances.at(file.m_game_version)->lookup_references(symbol_name);
  }
  return {};
}

std::vector<std::tuple<std::string, std::string, Docs::DefinitionLocation>>
Workspace::get_symbols_parent_type_path(const std::string& symbol_name,
                                        const GameVersion game_version) {
  if (m_compiler_instances.find(game_version) == m_compiler_instances.end()) {
    lg::debug("Compiler not instantiated for game version - {}",
              version_to_game_name(game_version));
    return {};
  }

  // name, docstring, def_loc
  std::vector<std::tuple<std::string, std::string, Docs::DefinitionLocation>> parents = {};

  const auto& compiler = m_compiler_instances[game_version].get();
  const auto parent_path = compiler->type_system().get_path_up_tree(symbol_name);
  for (const auto& parent : parent_path) {
    const auto symbol_infos = compiler->lookup_exact_name_info(parent);
    if (symbol_infos.empty()) {
      continue;
    }
    symbol_info::SymbolInfo* symbol_info;
    if (symbol_infos.size() > 1) {
      for (const auto& info : symbol_infos) {
        if (info->m_kind == symbol_info::Kind::TYPE) {
          symbol_info = info;
        }
      }
    } else {
      symbol_info = symbol_infos.at(0);
    }
    if (!symbol_info) {
      continue;
    }
    const auto& def_loc = symbol_info->m_def_location;
    if (!def_loc) {
      continue;
    }
    Docs::DefinitionLocation new_def_loc;
    new_def_loc.filename = lsp_util::uri_from_path(def_loc->file_path);
    new_def_loc.line_idx = def_loc->line_idx;
    new_def_loc.char_idx = def_loc->char_idx;
    parents.push_back({parent, symbol_info->m_docstring, new_def_loc});
  }
  return parents;
}

std::vector<std::tuple<std::string, std::string, Docs::DefinitionLocation>>
Workspace::get_types_subtypes(const std::string& symbol_name, const GameVersion game_version) {
  if (m_compiler_instances.find(game_version) == m_compiler_instances.end()) {
    lg::debug("Compiler not instantiated for game version - {}",
              version_to_game_name(game_version));
    return {};
  }

  // name, docstring, def_loc
  std::vector<std::tuple<std::string, std::string, Docs::DefinitionLocation>> subtypes = {};

  const auto& compiler = m_compiler_instances[game_version].get();
  const auto subtype_names =
      compiler->type_system().search_types_by_parent_type_strict(symbol_name);
  for (const auto& subtype_name : subtype_names) {
    const auto symbol_infos = compiler->lookup_exact_name_info(subtype_name);
    if (symbol_infos.empty()) {
      continue;
    } else if (symbol_infos.size() > 1) {
      continue;
    }
    const auto& symbol_info = symbol_infos.at(0);
    const auto& def_loc = symbol_info->m_def_location;
    if (!def_loc) {
      continue;
    }
    Docs::DefinitionLocation new_def_loc;
    new_def_loc.filename = lsp_util::uri_from_path(def_loc->file_path);
    new_def_loc.line_idx = def_loc->line_idx;
    new_def_loc.char_idx = def_loc->char_idx;
    subtypes.push_back({subtype_name, symbol_info->m_docstring, new_def_loc});
  }
  return subtypes;
}

std::unordered_map<std::string, s64> Workspace::get_enum_entries(const std::string& enum_name,
                                                                 const GameVersion game_version) {
  if (m_compiler_instances.find(game_version) == m_compiler_instances.end()) {
    lg::debug("Compiler not instantiated for game version - {}",
              version_to_game_name(game_version));
    return {};
  }

  const auto& compiler = m_compiler_instances[game_version].get();
  const auto enum_info = compiler->type_system().try_enum_lookup(enum_name);
  if (!enum_info) {
    return {};
  }
  return enum_info->entries();
}

bool Workspace::is_compiler_ready(const GameVersion game_version) const {
  return m_compiler_instances.find(game_version) != m_compiler_instances.end();
}

void Workspace::start_tracking_file(const LSPSpec::DocumentUri& file_uri,
                                    const std::string& language_id,
                                    const std::string& content) {
  auto norm_uri = normalize_uri(file_uri);
  lg::debug("start_tracking_file: raw URI: {}", file_uri);
  lg::debug("start_tracking_file: decoded path: {}", lsp_util::uri_to_path(file_uri));
  lg::debug("start_tracking_file: normalized key: {}", norm_uri);

  if (language_id == "opengoal-ir") {
    lg::debug("new ir file - {}", norm_uri);
    WorkspaceIRFile file(content);
    m_tracked_ir_files[norm_uri] = file;
  } else if (language_id == "opengoal") {
    if (m_tracked_og_files.find(norm_uri) != m_tracked_og_files.end()) {
      lg::debug("Already tracking - {}", norm_uri);
      return;
    }
    auto game_version = determine_game_version_from_uri(norm_uri);
    if (!game_version) {
      lg::debug("Could not determine game version from path - {}", norm_uri);
      return;
    }

    if (m_compiler_instances.find(*game_version) == m_compiler_instances.end()) {
      const auto game_name = version_to_game_name(*game_version);
      lg::info("indexing started for {}", game_name);
      lg::debug(
          "first time encountering a OpenGOAL file for game version - {}, initializing a compiler",
          game_name);
      const auto path = lsp_util::uri_to_path(norm_uri);
      const auto project_path =
          file_util::try_get_project_path_from_path(path);
      if (!project_path) {
        lg::warn("unable to find project path from {}, not initializing a compiler", path);
        return;
      }
      lg::info("Detected project root: {}", project_path.value());
      if (!file_util::setup_project_path(project_path)) {
        lg::debug("unable to setup project path, not initializing a compiler");
        return;
      }
      if (!work_done_progress_supported()) {
        lg::info("LSP workDoneProgress capability is disabled/unsupported");
      }
      const std::string progress_title =
          fmt::format("Compiling {}", version_to_game_name_external(game_version.value()));
      
      auto tracker = std::make_shared<lsp_util::CompileProgressTracker>(
          progress_title,
          [this, progress_title](const lsp_util::CompileProgressTracker::ProgressEvent& event) {
            if (event.kind == "create") {
              m_requester.send_progress_create_request(event.token, progress_title, event.message, event.percentage);
            } else if (event.kind == "report") {
              m_requester.send_progress_update_request(event.token, event.message, event.percentage);
            } else if (event.kind == "end") {
              m_requester.send_progress_finish_request(event.token, event.message);
            }
          },
          work_done_progress_supported());

      tracker->start("Starting OpenGOAL indexing");
      lg::add_print_callback([tracker](const std::string& line) {
        tracker->handle_chunk(line);
      });

      m_compiler_instances.emplace(
          game_version.value(),
          std::make_unique<Compiler>(game_version.value(), emitter::InstructionSet::X86));
      try {
        lg::info("starting initial indexing: make-group all-code");
        m_compiler_instances.at(*game_version)
            ->run_front_end_on_string("(make-group \"all-code\")");
        lg::info("indexing finished for {}", game_name);
        lg::clear_print_callbacks();
        tracker->finish("OpenGOAL indexing complete");
      } catch (std::exception& e) {
        lg::error("indexing failed for {}: {}", game_name, e.what());
        lg::clear_print_callbacks();
        tracker->finish("OpenGOAL indexing failed");
        lg::debug("error when {}", progress_title);

        auto parse_result = lsp_util::parse_compiler_error(e.what());
        if (parse_result.success && !parse_result.file_path.empty()) {
          auto target_uri = normalize_uri(lsp_util::uri_from_path(parse_result.file_path));
          // We can't easily assign it to m_tracked_og_files here if it's not THIS file
          // but we can at least log it or try to find it if it was just added
          if (target_uri == norm_uri) {
            // This will be handled after we emplace the file below, 
            // but we need a way to pass it down.
            // For now, let's just log it and rely on the next save to show it properly,
            // or we can store it in a temporary map if we really wanted to.
          }
        }
      }
    }
    m_tracked_og_files.emplace(norm_uri, WorkspaceOGFile(norm_uri, content, *game_version));
    scan_file_for_defines(m_tracked_og_files[norm_uri]);
    m_tracked_og_files[norm_uri].update_symbols(
        m_compiler_instances.at(*game_version)
            ->lookup_symbol_info_by_file(lsp_util::uri_to_path(norm_uri)));
  }
  lg::debug("tracked file count after adding: {}", track_file_count());
}

void Workspace::ensure_file_tracked(const LSPSpec::DocumentUri& file_uri,
                                    const std::optional<std::string>& language_id,
                                    const std::optional<std::string>& content) {
  auto norm_uri = normalize_uri(file_uri);
  if (m_tracked_og_files.find(norm_uri) != m_tracked_og_files.end() ||
      m_tracked_ir_files.find(norm_uri) != m_tracked_ir_files.end()) {
    return;
  }

  lg::info("Lazy tracking file: {}", norm_uri);

  std::string actual_content;
  if (content) {
    actual_content = *content;
  } else {
    auto path = lsp_util::uri_to_path(norm_uri);
    actual_content = file_util::read_text_file(path);
  }

  std::string actual_lang_id;
  if (language_id) {
    actual_lang_id = *language_id;
  } else {
    auto type = determine_filetype_from_uri(norm_uri);
    if (type == FileType::OpenGOAL) {
      actual_lang_id = "opengoal";
    } else if (type == FileType::OpenGOALIR) {
      actual_lang_id = "opengoal-ir";
    }
  }

  if (!actual_lang_id.empty()) {
    start_tracking_file(norm_uri, actual_lang_id, actual_content);
  } else {
    lg::warn("Could not determine language id for lazy tracking: {}", norm_uri);
  }
}

void Workspace::update_tracked_file(const LSPSpec::DocumentUri& file_uri,
                                    const std::string& content) {
  auto norm_uri = normalize_uri(file_uri);
  lg::debug("potentially updating - {}", norm_uri);
  // Check if the file is already tracked or not, this is done because change events don't give
  // language details it's assumed you are keeping track of that!
  if (m_tracked_ir_files.find(norm_uri) != m_tracked_ir_files.end()) {
    lg::debug("updating tracked IR file - {}", norm_uri);
    WorkspaceIRFile file(content);
    m_tracked_ir_files[norm_uri] = file;
  } else if (m_tracked_og_files.find(norm_uri) != m_tracked_og_files.end()) {
    lg::debug("updating tracked OG file - {}", norm_uri);
    m_tracked_og_files[norm_uri].parse_content(content);
    scan_file_for_defines(m_tracked_og_files[norm_uri]);
    // re-`ml` the file
    const auto game_version = m_tracked_og_files[norm_uri].m_game_version;
    if (m_compiler_instances.find(game_version) == m_compiler_instances.end()) {
      lg::debug("No compiler initialized for - {}", version_to_game_name(game_version));
      return;
    }
  }
}

void Workspace::handle_file_save(const LSPSpec::DocumentUri& file_uri) {
  auto norm_uri = normalize_uri(file_uri);
  lg::debug("file will be saved - {}", norm_uri);
  if (m_tracked_og_files.find(norm_uri) != m_tracked_og_files.end()) {
    // goalc is not an incremental compiler (yet) so I believe it will be a better UX
    // to re-compile on the file save, rather than as the user is typing
    const auto game_version = m_tracked_og_files[norm_uri].m_game_version;
    if (m_compiler_instances.find(game_version) == m_compiler_instances.end()) {
      lg::debug("No compiler initialized for - {}", version_to_game_name(game_version));
      return;
    }
    CompilationOptions options;
    options.filename = lsp_util::uri_to_path(norm_uri);
    lg::info("didSave recompiling file: {}", options.filename);

    if (!work_done_progress_supported()) {
      lg::info("LSP workDoneProgress capability is disabled/unsupported");
    }
    const std::string progress_title =
        fmt::format("Compiling {}", fs::path(options.filename).filename().string());
    auto tracker = std::make_shared<lsp_util::CompileProgressTracker>(
        progress_title,
        [this, progress_title](const lsp_util::CompileProgressTracker::ProgressEvent& event) {
          if (event.kind == "create") {
            m_requester.send_progress_create_request(event.token, progress_title, event.message, event.percentage);
          } else if (event.kind == "report") {
            m_requester.send_progress_update_request(event.token, event.message, event.percentage);
          } else if (event.kind == "end") {
            m_requester.send_progress_finish_request(event.token, event.message);
          }
        },
        work_done_progress_supported());

    tracker->start("Compiling file");
    lg::add_print_callback([tracker](const std::string& line) {
      tracker->handle_chunk(line);
    });

    try {
      // re-compile the file
      m_compiler_instances.at(game_version)->asm_file(options);
      // Update symbols for this specific file
      const auto symbol_infos =
          m_compiler_instances.at(game_version)->lookup_symbol_info_by_file(options.filename);
      m_tracked_og_files[norm_uri].update_symbols(symbol_infos);
      m_tracked_og_files[norm_uri].m_diagnostics.clear();
      lg::info("didSave recompile completed");
      lg::clear_print_callbacks();
      tracker->finish("Recompile complete");
    } catch (std::exception& e) {
      lg::error("didSave recompile failed: {}", e.what());
      lg::clear_print_callbacks();
      tracker->finish("Recompile failed");
      auto parse_result = lsp_util::parse_compiler_error(e.what());
      if (parse_result.success) {
        // If we found a file path in the error, try to use it, otherwise use current file
        auto target_uri = norm_uri;
        if (!parse_result.file_path.empty()) {
          auto target_path = fs::path(parse_result.file_path);
          if (target_path.is_relative()) {
            target_path = file_util::get_jak_project_dir() / target_path;
          }
          target_uri = normalize_uri(lsp_util::uri_from_path(target_path));
        }

        if (m_tracked_og_files.find(target_uri) != m_tracked_og_files.end()) {
          m_tracked_og_files[target_uri].m_diagnostics = {parse_result.diagnostic};
        } else {
          lg::warn("Compiler error in untracked file: {}", target_uri);
          // Fallback to current file if target is not tracked
          m_tracked_og_files[norm_uri].m_diagnostics = {parse_result.diagnostic};
        }
      } else {
        // Fallback: assign to current file at line 0 if parsing failed
        LSPSpec::Diagnostic diag;
        diag.m_message = e.what();
        diag.m_severity = LSPSpec::DiagnosticSeverity::Error;
        diag.m_range = LSPSpec::Range(0, 0);
        diag.m_source = "OpenGOAL";
        m_tracked_og_files[norm_uri].m_diagnostics = {diag};
      }
    }
  }
}

// clang-format off
void Workspace::update_global_index(const GameVersion game_version) {
  // TODO - project wide indexing potentially (ie. finding references)
};
// clang-format on

void Workspace::stop_tracking_file(const LSPSpec::DocumentUri& file_uri) {
  auto norm_uri = normalize_uri(file_uri);
  m_tracked_ir_files.erase(norm_uri);
  m_tracked_og_files.erase(norm_uri);
}

WorkspaceOGFile::WorkspaceOGFile(const LSPSpec::DocumentUri& uri,
                                 const std::string& content,
                                 const GameVersion& game_version)
    : m_uri(uri), m_game_version(game_version), version(0) {
  const auto [line_count, line_ending] =
      file_util::get_majority_file_line_endings_and_count(content);
  m_line_count = line_count;
  m_line_ending = line_ending;
  lg::info("Added new OG file. {} symbols and {} diagnostics", m_symbols.size(),
           m_diagnostics.size());
  parse_content(content);
}

void WorkspaceOGFile::parse_content(const std::string& content) {
  m_content = content;
  auto parser = ts_parser_new();
  if (ts_parser_set_language(parser, g_opengoalLang)) {
    // Get the AST for the current state of the file
    // TODO - eventually, we should consider doing partial updates of the AST
    // but right now the LSP just receives the entire document so that's a larger change.
    m_ast.reset(ts_parser_parse_string(parser, NULL, m_content.c_str(), m_content.length()),
                TreeSitterTreeDeleter());
  }
  ts_parser_delete(parser);
}

void WorkspaceOGFile::update_symbols(const std::vector<symbol_info::SymbolInfo*>& symbol_infos) {
  m_symbols.clear();
  // TODO - sorting by definition location would be nice (maybe VSCode already does this?)
  for (const auto& symbol_info : symbol_infos) {
    LSPSpec::DocumentSymbol lsp_sym;
    lsp_sym.m_name = symbol_info->m_name;
    lsp_sym.m_detail = symbol_info->m_docstring;
    switch (symbol_info->m_kind) {
      case symbol_info::Kind::CONSTANT:
        lsp_sym.m_kind = LSPSpec::SymbolKind::Constant;
        break;
      case symbol_info::Kind::FUNCTION:
        lsp_sym.m_kind = LSPSpec::SymbolKind::Function;
        break;
      case symbol_info::Kind::GLOBAL_VAR:
        lsp_sym.m_kind = LSPSpec::SymbolKind::Variable;
        break;
      case symbol_info::Kind::MACRO:
        lsp_sym.m_kind = LSPSpec::SymbolKind::Operator;
        break;
      case symbol_info::Kind::METHOD:
        lsp_sym.m_name = fmt::format("{}::{}", symbol_info->m_type, symbol_info->m_name);
        lsp_sym.m_kind = LSPSpec::SymbolKind::Method;
        break;
      case symbol_info::Kind::TYPE:
        lsp_sym.m_kind = LSPSpec::SymbolKind::Class;
        break;
      default:
        lsp_sym.m_kind = LSPSpec::SymbolKind::Object;
        break;
    }
    if (symbol_info->m_def_location) {
      lsp_sym.m_range = LSPSpec::Range(symbol_info->m_def_location->line_idx,
                                       symbol_info->m_def_location->char_idx);
    } else {
      lsp_sym.m_range = LSPSpec::Range(0, 0);
    }
    // TODO - would be nice to make this accurate but we don't store that info yet
    lsp_sym.m_selectionRange = lsp_sym.m_range;
    if (symbol_info->m_kind == symbol_info::Kind::TYPE) {
      std::vector<LSPSpec::DocumentSymbol> type_symbols = {};
      for (const auto& field : symbol_info->m_type_fields) {
        LSPSpec::DocumentSymbol field_sym;
        field_sym.m_name = field.name;
        field_sym.m_detail = field.description;
        if (field.is_array) {
          field_sym.m_kind = LSPSpec::SymbolKind::Array;
        } else {
          field_sym.m_kind = LSPSpec::SymbolKind::Field;
        }
        // TODO - we don't store the line number for fields
        field_sym.m_range = lsp_sym.m_range;
        field_sym.m_selectionRange = lsp_sym.m_selectionRange;
        type_symbols.push_back(field_sym);
      }
      for (const auto& method : symbol_info->m_type_methods) {
        LSPSpec::DocumentSymbol method_sym;
        method_sym.m_name = method.name;
        method_sym.m_kind = LSPSpec::SymbolKind::Method;
        // TODO - we don't store the line number for fields
        method_sym.m_range = lsp_sym.m_range;
        method_sym.m_selectionRange = lsp_sym.m_selectionRange;
        type_symbols.push_back(method_sym);
      }
      for (const auto& state : symbol_info->m_type_states) {
        LSPSpec::DocumentSymbol state_sym;
        state_sym.m_name = state.name;
        state_sym.m_kind = LSPSpec::SymbolKind::Event;
        // TODO - we don't store the line number for fields
        state_sym.m_range = lsp_sym.m_range;
        state_sym.m_selectionRange = lsp_sym.m_selectionRange;
        type_symbols.push_back(state_sym);
      }
      lsp_sym.m_children = type_symbols;
    }
    m_symbols.push_back(lsp_sym);
  }
}

std::optional<std::string> WorkspaceOGFile::get_symbol_at_position(
    const LSPSpec::Position position) const {
  if (m_ast) {
    TSNode root_node = ts_tree_root_node(m_ast.get());
    TSNode found_node =
        ts_node_descendant_for_point_range(root_node, {position.m_line, position.m_character},
                                           {position.m_line, position.m_character});
    if (!ts_node_has_error(found_node)) {
      uint32_t start = ts_node_start_byte(found_node);
      uint32_t end = ts_node_end_byte(found_node);
      const std::string node_str = m_content.substr(start, end - start);
      lg::debug("AST SAP - {}", node_str);
      const std::string node_name = ts_node_type(found_node);
      if (node_name == "sym_name") {
        return node_str;
      }
    } else {
      // found_node = ts_node_child(found_node, 0);
      // TODO - maybe get this one (but check if has an error)
      return {};
    }
  }
  return {};
}

TSNode WorkspaceOGFile::get_node_at_position(const LSPSpec::Position position) const {
  if (m_ast) {
    TSNode root_node = ts_tree_root_node(m_ast.get());
    TSNode node = ts_node_descendant_for_point_range(
        root_node, {position.m_line, position.m_character}, {position.m_line, position.m_character});

    // If we are on a paren or space, look slightly to the left
    if (std::string(ts_node_type(node)) != "sym_name" && position.m_character > 0) {
      TSNode left_node = ts_node_descendant_for_point_range(
          root_node, {position.m_line, position.m_character - 1},
          {position.m_line, position.m_character - 1});
      if (std::string(ts_node_type(left_node)) == "sym_name") {
        return left_node;
      }
    }
    return node;
  }
  return {{0, 0, 0, 0}};
}

std::vector<OpenGOALFormResult> WorkspaceOGFile::search_for_forms_that_begin_with(
    std::vector<std::string> prefix) const {
  std::vector<OpenGOALFormResult> results = {};
  if (!m_ast) {
    return results;
  }

  TSNode root_node = ts_tree_root_node(m_ast.get());
  std::vector<TSNode> found_nodes = {};
  ast_util::search_for_forms_that_begin_with(m_content, root_node, prefix, found_nodes);

  for (const auto& node : found_nodes) {
    std::vector<std::string> tokens = {};
    for (size_t i = 0; i < ts_node_child_count(node); i++) {
      const auto child_node = ts_node_child(node, i);
      const auto contents = ast_util::get_source_code(m_content, child_node);
      tokens.push_back(contents);
    }
    const auto start_point = ts_node_start_point(node);
    const auto end_point = ts_node_end_point(node);
    results.push_back(
        {tokens, {start_point.row, start_point.column}, {end_point.row, end_point.column}});
  }

  return results;
}

WorkspaceIRFile::WorkspaceIRFile(const std::string& content) {
  const auto line_ending = file_util::get_majority_file_line_endings(content);
  m_lines = str_util::split_string(content, line_ending);

  bool in_opengoal_block = false;
  for (size_t i = 0; i < m_lines.size(); i++) {
    const auto& line = m_lines.at(i);
    if (m_all_types_uri == "") {
      find_all_types_path(line);
    }
    if (str_util::contains(line, ";;-*-OpenGOAL-Start-*-")) {
      in_opengoal_block = true;
    } else if (str_util::contains(line, ";;-*-OpenGOAL-End-*-")) {
      in_opengoal_block = false;
    }
    find_function_symbol(i, line);
    identify_diagnostics(i, line, in_opengoal_block);
  }

  lg::info("Added new IR file. {} lines with {} symbols and {} diagnostics", m_lines.size(),
           m_symbols.size(), m_diagnostics.size());
}

// This is kind of a hack, but to ensure consistency.  The file will reference the all-types.gc
// file it was generated with, this lets us accurately jump to the definition properly!
void WorkspaceIRFile::find_all_types_path(const std::string& line) {
  std::regex regex("; ALL_TYPES=(.*)=(.*)");
  std::smatch matches;

  if (std::regex_search(line, matches, regex)) {
    if (matches.size() == 3) {
      const auto& game_version = matches[1];
      const auto& all_types_path = matches[2];
      lg::debug("Found DTS Path - {} : {}", game_version.str(), all_types_path.str());
      auto all_types_uri = lsp_util::uri_from_path(fs::path(all_types_path.str()));
      lg::debug("DTS URI - {}", all_types_uri);
      if (valid_game_version(game_version.str())) {
        m_game_version = game_name_to_version(game_version.str());
        m_all_types_uri = all_types_uri;
        m_all_types_file_path = fs::path(all_types_path.str());
      } else {
        lg::error("Invalid game version, ignoring - {}", game_version.str());
      }
    }
  }
}

void WorkspaceIRFile::find_function_symbol(const uint32_t line_num_zero_based,
                                           const std::string& line) {
  std::regex regex("; \\.function (.*)");
  std::smatch matches;

  if (std::regex_search(line, matches, regex)) {
    // NOTE - assumes we can only find 1 function per line
    if (matches.size() == 2) {
      const auto& match = matches[1];
      lg::info("Adding Symbol - {}", match.str());
      LSPSpec::DocumentSymbol new_symbol;
      new_symbol.m_name = match.str();
      new_symbol.m_kind = LSPSpec::SymbolKind::Function;
      LSPSpec::Range symbol_range;
      symbol_range.m_start = {line_num_zero_based, 0};
      symbol_range.m_end = {line_num_zero_based, 0};  // NOTE - set on the next function
      new_symbol.m_range = symbol_range;
      LSPSpec::Range symbol_selection_range;
      symbol_selection_range.m_start = {line_num_zero_based, 0};
      symbol_selection_range.m_end = {line_num_zero_based, (uint32_t)line.length() - 1};
      new_symbol.m_selectionRange = symbol_selection_range;
      m_symbols.push_back(new_symbol);
    }
  }

  std::regex end_function("^;; \\.endfunction\\s*$");
  if (std::regex_match(line, end_function)) {
    // Set the previous symbols end-line
    if (!m_symbols.empty()) {
      m_symbols[m_symbols.size() - 1].m_range.m_end.m_line = line_num_zero_based - 1;
    }
  }
}

void WorkspaceIRFile::identify_diagnostics(const uint32_t line_num_zero_based,
                                           const std::string& line,
                                           const bool /*in_opengoal_block*/) {
  std::regex info_regex(";; INFO: (.*)");
  std::regex warn_regex(";; WARN: (.*)");
  std::regex error_regex(";; ERROR: (.*)");
  std::smatch info_matches;
  std::smatch warn_matches;
  std::smatch error_matches;

  LSPSpec::Range diag_range;
  diag_range.m_start = {line_num_zero_based, 0};
  diag_range.m_end = {line_num_zero_based, (uint32_t)line.length() - 1};

  // Check for an info-level warnings
  if (std::regex_search(line, info_matches, info_regex)) {
    // NOTE - assumes we can only find 1 function per line
    if (info_matches.size() == 2) {
      const auto& match = info_matches[1];
      LSPSpec::Diagnostic new_diag;
      new_diag.m_severity = LSPSpec::DiagnosticSeverity::Information;
      new_diag.m_message = match.str();
      new_diag.m_range = diag_range;
      new_diag.m_source = "OpenGOAL LSP";
      m_diagnostics.push_back(new_diag);
      return;
    }
  }
  // Check for a warn level warnings
  if (std::regex_search(line, warn_matches, warn_regex)) {
    // NOTE - assumes we can only find 1 function per line
    if (warn_matches.size() == 2) {
      const auto& match = warn_matches[1];
      LSPSpec::Diagnostic new_diag;
      new_diag.m_severity = LSPSpec::DiagnosticSeverity::Warning;
      new_diag.m_message = match.str();
      new_diag.m_range = diag_range;
      new_diag.m_source = "OpenGOAL LSP";
      m_diagnostics.push_back(new_diag);
      return;
    }
  }

  // Check for a error level warnings
  if (std::regex_search(line, error_matches, error_regex)) {
    // NOTE - assumes we can only find 1 function per line
    if (error_matches.size() == 2) {
      const auto& match = error_matches[1];
      LSPSpec::Diagnostic new_diag;
      new_diag.m_severity = LSPSpec::DiagnosticSeverity::Error;
      new_diag.m_message = match.str();
      new_diag.m_range = diag_range;
      new_diag.m_source = "OpenGOAL LSP";
      m_diagnostics.push_back(new_diag);
      return;
    }
  }
}

std::optional<std::string> WorkspaceIRFile::get_mips_instruction_at_position(
    const LSPSpec::Position position) const {
  // Split the line on typical word boundaries
  std::string line = m_lines.at(position.m_line);
  std::smatch matches;
  std::regex regex("[\\w\\.]+");

  if (std::regex_search(line, matches, regex)) {
    const auto& match = matches[0];
    auto match_start = matches.position(0);
    auto match_end = match_start + match.length();
    if (position.m_character >= match_start && position.m_character <= match_end) {
      return match;
    }
  }

  return {};
}

std::optional<std::string> WorkspaceIRFile::get_symbol_at_position(
    const LSPSpec::Position position) const {
  // Split the line on typical word boundaries
  std::string line = m_lines.at(position.m_line);
  std::smatch matches;
  std::regex regex("[\\w\\.\\-_!<>*]+");
  std::regex_token_iterator<std::string::iterator> rend;

  std::regex_token_iterator<std::string::iterator> match(line.begin(), line.end(), regex);
  while (match != rend) {
    auto match_start = std::distance(line.begin(), match->first);
    auto match_end = match_start + match->length();
    if (position.m_character >= match_start && position.m_character <= match_end) {
      return match->str();
    }
    match++;
  }

  return {};
}

std::optional<LexicalBinding> find_lexical_binding(const WorkspaceOGFile& file, TSNode query_node, Workspace& workspace) {
  if (ts_node_is_null(query_node)) {
    return std::nullopt;
  }
  std::string query_name = ast_util::get_source_code(file.m_content, query_node);
  if (query_name.empty()) {
    return std::nullopt;
  }

  TSNode curr = ts_node_parent(query_node);
  while (!ts_node_is_null(curr)) {
    std::string curr_type = ts_node_type(curr);
    if (curr_type == "list" || curr_type == "form" || curr_type == "list_lit" || curr_type == "form_lit") {
      uint32_t named_count = ts_node_named_child_count(curr);
      if (named_count >= 2) {
        TSNode first_child = ts_node_named_child(curr, 0);
        std::string first_elt = ast_util::get_source_code(file.m_content, first_child);

        if (first_elt == "defbehavior") {
          if (query_name == "self" && named_count >= 4) {
            TSNode owner_type_node = ts_node_named_child(curr, 2);
            TSNode param_list = ts_node_named_child(curr, 3);
            if (ts_node_start_byte(query_node) >= ts_node_end_byte(param_list)) {
              std::string owner_type_str = ast_util::get_source_code(file.m_content, owner_type_node);
              return LexicalBinding{"self", "implicit self", owner_type_str, owner_type_node};
            }
          }
        } else if (first_elt == "defstate") {
          if (query_name == "self" && named_count >= 3) {
            TSNode type_list = ts_node_named_child(curr, 2);
            std::string tl_type = ts_node_type(type_list);
            if ((tl_type == "list" || tl_type == "form" || tl_type == "list_lit" || tl_type == "form_lit") &&
                ts_node_named_child_count(type_list) >= 1) {
              if (ts_node_start_byte(query_node) >= ts_node_end_byte(type_list)) {
                TSNode owner_type_node = ts_node_named_child(type_list, 0);
                std::string owner_type_str = ast_util::get_source_code(file.m_content, owner_type_node);
                return LexicalBinding{"self", "implicit self", owner_type_str, owner_type_node};
              }
            }
          }
        }

        if (first_elt == "defun" || first_elt == "defmethod" || first_elt == "defbehavior" || first_elt == "behavior") {
          TSNode param_list = {{0, 0, 0, 0}};
          for (uint32_t i = 1; i < named_count; i++) {
            TSNode child = ts_node_named_child(curr, i);
            std::string c_type = ts_node_type(child);
            if (c_type == "list" || c_type == "form" || c_type == "list_lit" || c_type == "form_lit") {
              param_list = child;
              break;
            }
          }

          if (!ts_node_is_null(param_list)) {
            uint32_t query_start = ts_node_start_byte(query_node);
            uint32_t param_end = ts_node_end_byte(param_list);
            if (query_start >= param_end) {
              uint32_t param_count = ts_node_named_child_count(param_list);
              for (uint32_t p = 0; p < param_count; p++) {
                TSNode param = ts_node_named_child(param_list, p);
                std::string p_type = ts_node_type(param);
                if (p_type == "list" || p_type == "form" || p_type == "list_lit" || p_type == "form_lit") {
                  if (ts_node_named_child_count(param) >= 2) {
                    TSNode p_name_node = ts_node_named_child(param, 0);
                    std::string p_name = ast_util::get_source_code(file.m_content, p_name_node);
                    if (p_name == query_name) {
                      TSNode p_type_node = ts_node_named_child(param, 1);
                      std::string p_type_str = ast_util::get_source_code(file.m_content, p_type_node);
                      return LexicalBinding{p_name, "parameter", p_type_str, p_name_node};
                    }
                  }
                } else {
                  std::string p_name = ast_util::get_source_code(file.m_content, param);
                  if (p_name == query_name) {
                    return LexicalBinding{p_name, "parameter", "", param};
                  }
                }
              }
            }
          }
        } else if (first_elt == "let" || first_elt == "let*") {
          TSNode bindings_list = ts_node_named_child(curr, 1);
          if (!ts_node_is_null(bindings_list) && ts_node_named_child_count(bindings_list) > 0) {
            uint32_t query_start = ts_node_start_byte(query_node);
            uint32_t bindings_end = ts_node_end_byte(bindings_list);

            int32_t binding_count = (int32_t)ts_node_named_child_count(bindings_list);
            for (int32_t b = binding_count - 1; b >= 0; b--) {
              TSNode binding = ts_node_named_child(bindings_list, b);
              if (ts_node_named_child_count(binding) >= 2) {
                TSNode var_node = ts_node_named_child(binding, 0);
                std::string var_name = ast_util::get_source_code(file.m_content, var_node);
                if (var_name == query_name) {
                  bool is_decl = (ts_node_start_byte(query_node) >= ts_node_start_byte(var_node) &&
                                  ts_node_end_byte(query_node) <= ts_node_end_byte(var_node));
                  bool in_scope = false;
                  if (is_decl) {
                    in_scope = true;
                  } else if (query_start >= bindings_end) {
                    in_scope = true;
                  } else if (first_elt == "let*" && query_start >= ts_node_end_byte(binding)) {
                    in_scope = true;
                  }

                  if (in_scope) {
                    TSNode init_node = ts_node_named_child(binding, 1);
                    std::string inferred_type = infer_type(file, init_node, workspace);
                    return LexicalBinding{var_name, "local", inferred_type, var_node};
                  }
                }
              }
            }
          }
        } else if (first_elt == "dotimes") {
          TSNode header = ts_node_named_child(curr, 1);
          if (!ts_node_is_null(header) && ts_node_named_child_count(header) >= 1) {
            uint32_t query_start = ts_node_start_byte(query_node);
            TSNode var_node = ts_node_named_child(header, 0);
            std::string var_name = ast_util::get_source_code(file.m_content, var_node);
            if (var_name == query_name && query_start >= ts_node_end_byte(var_node)) {
              return LexicalBinding{var_name, "local", "int", var_node};
            }
          }
        }
      }
    }
    curr = ts_node_parent(curr);
  }
  return std::nullopt;
}

std::string infer_type(const WorkspaceOGFile& file, TSNode node, Workspace& workspace) {
  if (ts_node_is_null(node)) return "";

  // Normalize node: if we are on a sym_name, move up to sym_lit if it exists
  if (std::string(ts_node_type(node)) == "sym_name") {
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent) && std::string(ts_node_type(parent)) == "sym_lit") {
      node = parent;
    }
  }

  std::string node_type = ts_node_type(node);
  std::string text = ast_util::get_source_code(file.m_content, node);

  if (node_type == "sym_name" || node_type == "sym_lit") {
    std::string sym_name = ast_util::get_source_code(file.m_content, node);

    // Check local bindings first to ensure local scope overrides global
    auto local_binding = find_lexical_binding(file, node, workspace);
    if (local_binding && !local_binding->type.empty()) {
      return local_binding->type;
    }

    auto global_type = workspace.get_symbol_typeinfo(file, sym_name);
    if (global_type) {
      return global_type->first.base_type();
    }
  }

  if (node_type == "str_lit" || (text.size() >= 2 && text.front() == '"' && text.back() == '"')) {
    return "string";
  }
  if (text == "#t" || text == "#f") {
    return "symbol";
  }
  std::regex float_regex("^-?[0-9]+\\.[0-9]+$");
  if (node_type == "float_lit" || node_type == "float" || std::regex_match(text, float_regex)) {
    return "float";
  }
  std::regex int_regex("^-?[0-9]+$");
  if (node_type == "int_lit" || node_type == "integer" || std::regex_match(text, int_regex)) {
    return "int";
  }

  if (node_type == "list" || node_type == "form" || node_type == "list_lit" || node_type == "form_lit") {
    uint32_t named_count = ts_node_named_child_count(node);
    if (named_count >= 1) {
      TSNode first_child = ts_node_named_child(node, 0);
      std::string first_elt = ast_util::get_source_code(file.m_content, first_child);

      if ((first_elt == "the-as" || first_elt == "the") && named_count >= 3) {
        TSNode type_node = ts_node_named_child(node, 1);
        return ast_util::get_source_code(file.m_content, type_node);
      }

      if (first_elt == "new" && named_count >= 3) {
        TSNode type_node = ts_node_named_child(node, 2);
        std::string type_text = ast_util::get_source_code(file.m_content, type_node);
        if (type_text.size() > 1 && type_text.front() == '\'') {
          type_text = type_text.substr(1);
        }
        return type_text;
      }

      if (first_elt == "->") {
        TSNode receiver_node = ts_node_named_child(node, 1);
        std::string receiver_type = infer_type(file, receiver_node, workspace);
        if (!receiver_type.empty()) {
          std::string curr_type = receiver_type;
          for (uint32_t i = 2; i < named_count; i++) {
            TSNode step = ts_node_named_child(node, i);
            std::string step_type = ts_node_type(step);
            if (step_type == "sym_name" || step_type == "sym_lit") {
              std::string step_name = ast_util::get_source_code(file.m_content, step);
              auto field_info = workspace.get_field_info(file, curr_type, step_name);
              if (field_info) {
                curr_type = field_info->type;
              }
            }
          }
          return curr_type;
        }
      }

      auto typeinfo = workspace.get_symbol_typeinfo(file, first_elt);
      if (typeinfo) {
        const auto& ts = typeinfo->first;
        if (ts.base_type() == "function" && ts.arg_count() > 0) {
          return ts.last_arg().base_type();
        }
      }
    }
  }

  return "";
}

bool is_under_arrow_field_pos(TSNode node, const WorkspaceOGFile& file) {
  TSNode curr = node;
  if (!ts_node_is_null(curr) && std::string(ts_node_type(curr)) == "sym_name") {
    TSNode parent = ts_node_parent(curr);
    if (!ts_node_is_null(parent) && std::string(ts_node_type(parent)) == "sym_lit") {
      curr = parent;
    }
  }

  TSNode p = ts_node_parent(curr);
  int depth = 0;
  while (!ts_node_is_null(p) && depth < 3) {
    std::string curr_type = ts_node_type(p);
    if (curr_type == "list" || curr_type == "form" || curr_type == "list_lit" || curr_type == "form_lit") {
      TSNode first_symbol = {{0, 0, 0, 0}};
      uint32_t search_limit = std::min(ts_node_child_count(p), (uint32_t)3);
      for (uint32_t i = 0; i < search_limit; i++) {
        TSNode child = ts_node_child(p, i);
        std::string c_type = ts_node_type(child);
        if (c_type == "sym_name" || c_type == "sym_lit") {
          first_symbol = child;
          break;
        }
      }

      if (!ts_node_is_null(first_symbol)) {
        std::string first_elt = ast_util::get_source_code(file.m_content, first_symbol);
        if (first_elt == "->") {
          std::vector<TSNode> sym_nodes;
          int my_sym_idx = -1;
          for (uint32_t i = 0; i < ts_node_child_count(p); i++) {
            TSNode child = ts_node_child(p, i);
            std::string c_type = ts_node_type(child);
            if (c_type == "sym_name" || c_type == "sym_lit") {
              if (ts_node_eq(child, curr)) {
                my_sym_idx = (int)sym_nodes.size();
              }
              sym_nodes.push_back(child);
            }
          }
          if (my_sym_idx >= 2) {
            return true;
          }
        }
      }
    }
    p = ts_node_parent(p);
    depth++;
  }
  return false;
}

std::string infer_global_define_type(const WorkspaceOGFile& /*file*/, const symbol_info::SymbolInfo* /*sym_info*/, Workspace& /*workspace*/) {
  return "";
}

void scan_for_defines_rec(const WorkspaceOGFile& file, TSNode node, Workspace& workspace) {
  if (ts_node_is_null(node)) return;

  std::string node_type = ts_node_type(node);
  if (node_type == "list" || node_type == "form" || node_type == "list_lit" || node_type == "form_lit") {
    uint32_t named_count = ts_node_named_child_count(node);
    if (named_count >= 3) {
      TSNode first_child = ts_node_named_child(node, 0);
      std::string first_elt = ast_util::get_source_code(file.m_content, first_child);
      if (first_elt == "define") {
        TSNode name_node = ts_node_named_child(node, 1);
        TSNode init_node = ts_node_named_child(node, 2);
        
        std::string name_type = ts_node_type(name_node);
        if (name_type == "sym_name" || name_type == "sym_lit") {
          std::string name_str = ast_util::get_source_code(file.m_content, name_node);
          if (name_str.size() > 1 && name_str.front() == '\'') {
            name_str = name_str.substr(1);
          }
          std::string inferred = infer_type(file, init_node, workspace);
          if (!inferred.empty()) {
            workspace.m_inferred_global_types[file.m_game_version][name_str] = inferred;
            lg::info("plain define inference: {} -> {}", name_str, inferred);
          }
        }
      }
    }
  }

  uint32_t child_count = ts_node_child_count(node);
  for (uint32_t i = 0; i < child_count; i++) {
    scan_for_defines_rec(file, ts_node_child(node, i), workspace);
  }
}

void Workspace::scan_file_for_defines(const WorkspaceOGFile& file) {
  if (file.get_ast()) {
    scan_for_defines_rec(file, ts_tree_root_node(file.get_ast().get()), *this);
  }
}
