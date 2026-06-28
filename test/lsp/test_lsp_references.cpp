#include "gtest/gtest.h"

#include <algorithm>
#include <string>
#include <vector>

#include "goalc/compiler/Compiler.h"
#include "lsp/handlers/text_document/references.h"
#include "lsp/protocol/common_types.h"
#include "lsp/state/workspace.h"

namespace {

struct MarkedFixture {
  std::string code;
  std::vector<LSPSpec::Position> marks;
};

MarkedFixture strip_marks(const std::string& marked_code) {
  MarkedFixture result;
  LSPSpec::Position pos{0, 0};
  for (char c : marked_code) {
    if (c == '|') {
      result.marks.push_back(pos);
      continue;
    }
    result.code.push_back(c);
    if (c == '\n') {
      pos.m_line++;
      pos.m_character = 0;
    } else {
      pos.m_character++;
    }
  }
  return result;
}

LSPSpec::Range token_range(LSPSpec::Position start, const std::string& token) {
  LSPSpec::Range range;
  range.m_start = start;
  range.m_end = {start.m_line, start.m_character + (uint32_t)token.length()};
  return range;
}

std::string range_key(const LSPSpec::Location& location) {
  return std::to_string(location.m_range.m_start.m_line) + ":" +
         std::to_string(location.m_range.m_start.m_character) + "-" +
         std::to_string(location.m_range.m_end.m_line) + ":" +
         std::to_string(location.m_range.m_end.m_character);
}

class LSPReferencesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    workspace = std::make_unique<Workspace>();
    workspace->set_initialized(true);
    compiler = std::make_unique<Compiler>(GameVersion::Jak2, emitter::InstructionSet::X86);
    file_uri = "file:///goal_src/jak2/multiplayer/reference-fixture.gc";
  }

  void load_code(const std::string& marked_code, bool compile = true) {
    fixture = strip_marks(marked_code);
    ASSERT_FALSE(fixture.marks.empty());
    if (compile) {
      compiler->run_front_end_on_string(fixture.code);
    }
    workspace->inject_compiler(GameVersion::Jak2, std::move(compiler));
    WorkspaceOGFile file(file_uri, fixture.code, GameVersion::Jak2);
    workspace->inject_tracked_og_file(file_uri, file);
  }

  std::vector<LSPSpec::Location> refs_at(size_t mark_index, bool include_declaration) {
    LSPSpec::ReferenceParams params;
    params.m_textDocument.m_uri = file_uri;
    params.m_position = fixture.marks.at(mark_index);
    params.m_context.m_includeDeclaration = include_declaration;

    auto res = lsp_handlers::references(*workspace, 1, params);
    EXPECT_TRUE(res.has_value());
    if (!res.has_value()) {
      return {};
    }
    auto locations = res.value().get<std::vector<LSPSpec::Location>>();
    std::sort(locations.begin(), locations.end(), [](const auto& a, const auto& b) {
      if (a.m_range.m_start.m_line != b.m_range.m_start.m_line) {
        return a.m_range.m_start.m_line < b.m_range.m_start.m_line;
      }
      return a.m_range.m_start.m_character < b.m_range.m_start.m_character;
    });
    return locations;
  }

  void expect_refs(const std::vector<LSPSpec::Location>& locations,
                   const std::vector<size_t>& mark_indices,
                   const std::string& token) {
    ASSERT_EQ(locations.size(), mark_indices.size());
    for (size_t i = 0; i < mark_indices.size(); i++) {
      const auto expected = token_range(fixture.marks.at(mark_indices.at(i)), token);
      EXPECT_EQ(locations.at(i).m_uri, file_uri);
      EXPECT_EQ(range_key(locations.at(i)), range_key(LSPSpec::Location{file_uri, expected}));
    }
  }

  std::unique_ptr<Workspace> workspace;
  std::unique_ptr<Compiler> compiler;
  std::string file_uri;
  MarkedFixture fixture;
};

TEST_F(LSPReferencesTest, GlobalReferencesIncludeAndExcludeDeclaration) {
  load_code(R"(
(define |*active-pool* (new 'global 'process-tree "active-pool"))

(defun use-a ()
  |*active-pool*)

(defun use-b ()
  (format 0 "~A" |*active-pool*))
)",
            false);

  expect_refs(refs_at(0, true), {0, 1, 2}, "*active-pool*");
  expect_refs(refs_at(1, true), {0, 1, 2}, "*active-pool*");
  expect_refs(refs_at(1, false), {1, 2}, "*active-pool*");
}

TEST_F(LSPReferencesTest, FunctionReferences) {
  load_code(R"(
(defun |helper ()
  (none))

(defun use-a ()
  (|helper))

(defun use-b ()
  (|helper))
)");

  expect_refs(refs_at(0, true), {0, 1, 2}, "helper");
  expect_refs(refs_at(1, false), {1, 2}, "helper");
}

TEST_F(LSPReferencesTest, LetLocalReferencesDoNotEscapeScope) {
  load_code(R"(
(defun test ()
  (let ((|proc (process-by-name name-str *active-pool*)))
    (when |proc
      |proc)))

(defun other ()
  (let ((|proc 0))
    |proc))
)", false);

  expect_refs(refs_at(0, true), {0, 1, 2}, "proc");
  expect_refs(refs_at(1, false), {1, 2}, "proc");
  expect_refs(refs_at(3, true), {3, 4}, "proc");
}

TEST_F(LSPReferencesTest, LetShadowing) {
  load_code(R"(
(defun test ()
  (let ((|x 1))
    |x
    (let ((|x 2))
      |x)
    |x))
)", false);

  expect_refs(refs_at(0, true), {0, 1, 4}, "x");
  expect_refs(refs_at(2, true), {2, 3}, "x");
}

TEST_F(LSPReferencesTest, LetStarReferences) {
  load_code(R"(
(defun test ()
  (let* ((|a 1)
         (|b |a))
    |b))
)", false);

  expect_refs(refs_at(0, true), {0, 2}, "a");
  expect_refs(refs_at(1, true), {1, 3}, "b");
}

TEST_F(LSPReferencesTest, ParameterReferencesDoNotConfuseFields) {
  load_code(R"(
(deftype mp-battle-event (basic)
  ((initial? int)))

(defun test ((|evt mp-battle-event))
  (-> |evt initial?)
  |evt)
)");

  expect_refs(refs_at(1, true), {0, 1, 2}, "evt");
  expect_refs(refs_at(1, false), {1, 2}, "evt");
}

TEST_F(LSPReferencesTest, FieldReferencesByReceiverType) {
  load_code(R"(
(deftype mp-battle-event (basic)
  ((|initial? int)
   (breed-index int)))

(deftype other-event (basic)
  ((|initial? int)))

(defun test ((evt mp-battle-event) (other other-event))
  (-> evt |initial?)
  (-> evt |initial?)
  (-> other |initial?))
)");

  expect_refs(refs_at(0, true), {0, 2, 3}, "initial?");
  expect_refs(refs_at(2, false), {2, 3}, "initial?");
  expect_refs(refs_at(1, true), {1, 4}, "initial?");
}

TEST_F(LSPReferencesTest, TypeReferences) {
  load_code(R"(
(deftype |target (basic) ())

(defun test ((x |target))
  x)

(define *target-instance* (the-as |target #f))
)");

  expect_refs(refs_at(0, true), {0, 1, 2}, "target");
  expect_refs(refs_at(1, false), {1, 2}, "target");
}

TEST_F(LSPReferencesTest, BehaviorReferencesPreferConcreteDefinitionOverExtern) {
  load_code(R"(
(deftype target (basic) ())

(defbehavior |target-gun-fire-blue target ()
  (none))

(define-extern |target-gun-fire-blue (function (pointer process) :behavior target))

(defun call-behavior ()
  (|target-gun-fire-blue))
)",
            false);

  expect_refs(refs_at(0, true), {0, 2}, "target-gun-fire-blue");
  expect_refs(refs_at(2, false), {2}, "target-gun-fire-blue");
}

}  // namespace
