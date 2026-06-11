#include "gtest/gtest.h"
#include "lsp/state/workspace.h"
#include "lsp/handlers/text_document/hover.h"
#include "lsp/handlers/text_document/go_to.h"
#include "lsp/protocol/common_types.h"
#include "lsp/protocol/hover.h"
#include "lsp/lsp_util.h"
#include "goalc/compiler/Compiler.h"

class LSPHoverDefinitionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Initialize compiler for Jak2
    compiler = std::make_unique<Compiler>(GameVersion::Jak2, emitter::InstructionSet::X86);
    
    // Set up standard types/globals used in the test fixture
    compiler->run_front_end_on_string(R"(
(deftype mp-battle-event (basic)
  ((spawner-index int)
   (breed-index int)
   (initial? int)
   (battle-name string)
   (battle-evt int)))
)");
    compiler->run_front_end_on_string("(define-extern *mp-mission-event-name-scratch* string)");
    compiler->run_front_end_on_string("(define-extern *foo* int)");

    workspace = std::make_unique<Workspace>();
    workspace->set_initialized(true);
    workspace->inject_compiler(GameVersion::Jak2, std::move(compiler));

    file_uri = "file:///goal_src/jak2/multiplayer/core/test-fixture.gc";
    
    test_code = R"(
(deftype mp-battle-event (basic)
  ((spawner-index int)
   (breed-index int)
   (initial? int)
   (battle-name string)
   (battle-evt int)))

(defglobal *mp-mission-event-name-scratch* string)

(defun mp-receive-battle-event ((evt mp-battle-event))
  (let ((battle-evt (-> evt battle-evt))
        (name-str *mp-mission-event-name-scratch*))
    (send-event
      name-str
      battle-evt
      (the-as int (-> evt breed-index))
      (if (zero? (-> evt initial?)) #f #t))))
)";

    WorkspaceOGFile file(file_uri, test_code, GameVersion::Jak2);
    workspace->inject_tracked_og_file(file_uri, file);
  }

  std::unique_ptr<Workspace> workspace;
  std::unique_ptr<Compiler> compiler;
  std::string file_uri;
  std::string test_code;
};

// Test 1: hover parameter
TEST_F(LSPHoverDefinitionTest, HoverParameter) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 16;
  params.m_position.m_character = 22; // points to "evt" in (-> evt breed-index)

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("evt"), std::string::npos);
  EXPECT_NE(text.find("Parameter"), std::string::npos);
  EXPECT_NE(text.find("mp-battle-event"), std::string::npos);
}

// Test 2: definition parameter
TEST_F(LSPHoverDefinitionTest, DefinitionParameter) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 16;
  params.m_position.m_character = 22; // points to "evt" in (-> evt breed-index)

  auto res = lsp_handlers::go_to_definition(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto locations = res.value().get<std::vector<LSPSpec::Location>>();
  ASSERT_EQ(locations.size(), 1);
  // Expected declaration: ((evt mp-battle-event)) on line 10, char 33
  EXPECT_EQ(locations[0].m_range.m_start.m_line, 10);
  EXPECT_EQ(locations[0].m_range.m_start.m_character, 33);
}

// Test 3: hover let local
TEST_F(LSPHoverDefinitionTest, HoverLetLocal) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 15;
  params.m_position.m_character = 6; // points to "battle-evt" in send-event call

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("battle-evt"), std::string::npos);
  EXPECT_NE(text.find("Local variable"), std::string::npos);
  EXPECT_NE(text.find("int"), std::string::npos); // inferred type from (-> evt battle-evt)
}

// Test 4: definition let local
TEST_F(LSPHoverDefinitionTest, DefinitionLetLocal) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 15;
  params.m_position.m_character = 6; // points to "battle-evt" in send-event call

  auto res = lsp_handlers::go_to_definition(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto locations = res.value().get<std::vector<LSPSpec::Location>>();
  ASSERT_EQ(locations.size(), 1);
  // Expected declaration: (battle-evt (-> evt battle-evt)) on line 11, char 9
  EXPECT_EQ(locations[0].m_range.m_start.m_line, 11);
  EXPECT_EQ(locations[0].m_range.m_start.m_character, 9);
}

// Test 5: hover local initialized from global
TEST_F(LSPHoverDefinitionTest, HoverLocalFromGlobal) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 14;
  params.m_position.m_character = 6; // points to "name-str" in send-event call

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("name-str"), std::string::npos);
  EXPECT_NE(text.find("Local variable"), std::string::npos);
  EXPECT_NE(text.find("string"), std::string::npos); // inferred type from global
}

// Test 6: definition local initialized from global
TEST_F(LSPHoverDefinitionTest, DefinitionLocalFromGlobal) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 14;
  params.m_position.m_character = 6; // points to "name-str" in send-event call

  auto res = lsp_handlers::go_to_definition(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto locations = res.value().get<std::vector<LSPSpec::Location>>();
  ASSERT_EQ(locations.size(), 1);
  // Expected declaration: (name-str *mp-mission-event-name-scratch*) on line 12, char 9
  EXPECT_EQ(locations[0].m_range.m_start.m_line, 12);
  EXPECT_EQ(locations[0].m_range.m_start.m_character, 9);
}

// Test 7: hover field access
TEST_F(LSPHoverDefinitionTest, HoverFieldAccess) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 17;
  params.m_position.m_character = 25; // points to "initial?" in (-> evt initial?)

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("initial?"), std::string::npos);
  EXPECT_NE(text.find("Field"), std::string::npos);
  EXPECT_NE(text.find("mp-battle-event"), std::string::npos);
  EXPECT_NE(text.find("int"), std::string::npos);
}

// Test 8: definition field access
TEST_F(LSPHoverDefinitionTest, DefinitionFieldAccess) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 17;
  params.m_position.m_character = 25; // points to "initial?" in (-> evt initial?)

  auto res = lsp_handlers::go_to_definition(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto locations = res.value().get<std::vector<LSPSpec::Location>>();
  // Should jump to field declaration inside deftype mp-battle-event
  ASSERT_EQ(locations.size(), 1);
  EXPECT_EQ(locations[0].m_range.m_start.m_line, 4);
}

// Test 9: nested field access with index (battle-name)
TEST_F(LSPHoverDefinitionTest, HoverNestedFieldAccessWithIndex) {
  std::string index_code = R"(
(defun test-nested ((evt mp-battle-event) (i int))
  (-> evt battle-name i))
)";
  WorkspaceOGFile file(file_uri, index_code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 2;
  params.m_position.m_character = 13; // points to "battle-name" in (-> evt battle-name i)

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("battle-name"), std::string::npos);
  EXPECT_NE(text.find("string"), std::string::npos);
  EXPECT_NE(text.find("mp-battle-event"), std::string::npos);
}

// Test 10: unresolved field fallback
TEST_F(LSPHoverDefinitionTest, UnresolvedFieldFallback) {
  std::string bad_code = R"(
(defun test-unresolved ((evt mp-battle-event))
  (-> evt does-not-exist))
)";
  WorkspaceOGFile file(file_uri, bad_code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 2;
  params.m_position.m_character = 13; // points to "does-not-exist"

  auto res_hover = lsp_handlers::hover(*workspace, 1, params);
  // Hover should return no info or empty/unresolved response, but not crash
  if (res_hover.has_value()) {
    auto hover = res_hover.value().get<LSPSpec::Hover>();
    EXPECT_EQ(hover.m_contents.m_value.find("does-not-exist"), std::string::npos);
  }

  auto res_def = lsp_handlers::go_to_definition(*workspace, 1, params);
  // Definition should not crash and either be empty or return no locations
  if (res_def.has_value()) {
    auto locations = res_def.value().get<std::vector<LSPSpec::Location>>();
    EXPECT_TRUE(locations.empty());
  }
}

// Test 11: Avoid breaking existing global behavior (regression)
TEST_F(LSPHoverDefinitionTest, GlobalRegression) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 8;
  params.m_position.m_character = 12; // points to "*mp-mission-event-name-scratch*" in (defglobal ...)

  // For global regression, check if we get a hover or definition.
  // Hover global -> global type "string"
  auto res = lsp_handlers::hover(*workspace, 1, params);
  if (res.has_value()) {
    auto hover = res.value().get<LSPSpec::Hover>();
    EXPECT_NE(hover.m_contents.m_value.find("string"), std::string::npos);
  }
}

class LSPLetInferenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    compiler = std::make_unique<Compiler>(GameVersion::Jak2, emitter::InstructionSet::X86);
    
    compiler->run_front_end_on_string(R"(
(deftype process-tree (basic) ())
(deftype process (process-tree) ())
(deftype battle (process) ())
(define-extern process-by-name (function string process-tree process))
(define-extern *active-pool* process-tree)
)");

    workspace = std::make_unique<Workspace>();
    workspace->set_initialized(true);
    workspace->inject_compiler(GameVersion::Jak2, std::move(compiler));

    file_uri = "file:///goal_src/jak2/multiplayer/test-inference.gc";
    
    test_code = R"(
(defun test-func ((name-str string))
  (let ((proc (process-by-name name-str *active-pool*)))
    (when (and proc (type? proc battle))
      proc)))
)";

    WorkspaceOGFile file(file_uri, test_code, GameVersion::Jak2);
    workspace->inject_tracked_og_file(file_uri, file);
  }

  std::unique_ptr<Workspace> workspace;
  std::unique_ptr<Compiler> compiler;
  std::string file_uri;
  std::string test_code;
};

// Test 1: hover function return type still works
TEST_F(LSPLetInferenceTest, HoverFunctionReturnType) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 2;
  params.m_position.m_character = 20; // points to "process-by-name"

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("Function"), std::string::npos);
  EXPECT_NE(text.find("process-by-name"), std::string::npos);
  EXPECT_NE(text.find("process"), std::string::npos);
}

// Test 2: hover let binding declaration
TEST_F(LSPLetInferenceTest, HoverLetBindingDeclaration) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 2;
  params.m_position.m_character = 10; // points to "proc" declaration

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("proc"), std::string::npos);
  EXPECT_NE(text.find("Local variable"), std::string::npos);
  EXPECT_NE(text.find("process"), std::string::npos);
}

// Test 3: hover let binding reference
TEST_F(LSPLetInferenceTest, HoverLetBindingReference) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 3;
  params.m_position.m_character = 15; // points to "proc" in (and proc ...)

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("proc"), std::string::npos);
  EXPECT_NE(text.find("Local variable"), std::string::npos);
  EXPECT_NE(text.find("process"), std::string::npos);
}

// Test 4: definition for local reference
TEST_F(LSPLetInferenceTest, DefinitionForLocalReference) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 3;
  params.m_position.m_character = 15; // points to "proc" in (and proc ...)

  auto res = lsp_handlers::go_to_definition(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto locations = res.value().get<std::vector<LSPSpec::Location>>();
  ASSERT_EQ(locations.size(), 1);
  EXPECT_EQ(locations[0].m_range.m_start.m_line, 2);
  EXPECT_EQ(locations[0].m_range.m_start.m_character, 9);
}

// Test 5: function call initializer should not make the callee local
TEST_F(LSPLetInferenceTest, FunctionCallInitializerNotLocal) {
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 2;
  params.m_position.m_character = 20; // points to "process-by-name"

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_EQ(text.find("Local variable"), std::string::npos);
  EXPECT_NE(text.find("Function"), std::string::npos);
}

// Test 6: unknown return type fallback
TEST_F(LSPLetInferenceTest, UnknownTypeFallback) {
  std::string fallback_code = R"(
(defun test-fallback ((arg int))
  (let ((x (some-unknown-function arg)))
    x))
)";
  WorkspaceOGFile file(file_uri, fallback_code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 3;
  params.m_position.m_character = 4; // points to "x" on the last line

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("x"), std::string::npos);
  EXPECT_NE(text.find("Local variable"), std::string::npos);
  EXPECT_NE(text.find("unknown"), std::string::npos);
}

// Test 7: nested let using inferred local
TEST_F(LSPLetInferenceTest, NestedLet) {
  std::string nested_code = R"(
(defun test-nested ((name-str string))
  (let ((proc (process-by-name name-str *active-pool*)))
    (let ((same-proc proc))
      same-proc)))
)";
  WorkspaceOGFile file(file_uri, nested_code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 4;
  params.m_position.m_character = 6; // points to "same-proc" on the last line

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("same-proc"), std::string::npos);
  EXPECT_NE(text.find("Local variable"), std::string::npos);
  EXPECT_NE(text.find("process"), std::string::npos);
}

class LSPImplicitSelfAndLiteralTest : public ::testing::Test {
 protected:
  void SetUp() override {
    compiler = std::make_unique<Compiler>(GameVersion::Jak2, emitter::InstructionSet::X86);
    
    compiler->run_front_end_on_string(R"(
(deftype transform (basic) ())
(deftype enemy (basic)
  ((root transform)))
(deftype target (basic)
  ((state-time float)))
(define-extern *target* target)
(deftype process-tree (basic) ())
(defmethod new process-tree ((allocation symbol) (type-to-make type) (name string)) process-tree)
(deftype process (process-tree) ())
(define-extern process-by-name (function string process-tree process))
(define-extern *active-pool* process-tree)
)");

    workspace = std::make_unique<Workspace>();
    workspace->set_initialized(true);
    workspace->inject_compiler(GameVersion::Jak2, std::move(compiler));

    file_uri = "file:///goal_src/jak2/multiplayer/test-extended.gc";
  }

  std::unique_ptr<Workspace> workspace;
  std::unique_ptr<Compiler> compiler;
  std::string file_uri;
};

// Test 1: self in defbehavior
TEST_F(LSPImplicitSelfAndLiteralTest, DefBehaviorSelfAndFields) {
  std::string code = R"(
(deftype enemy (basic)
  ((root transform)))

(defbehavior enemy-simple-post enemy ()
  (common-post self)
  (update-transforms (-> self root))
  0
  (none))
)";
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  // Hover on self in (common-post self) on line 5 (0-indexed), char 15
  LSPSpec::TextDocumentPositionParams params_self;
  params_self.m_textDocument.m_uri = file_uri;
  params_self.m_position.m_line = 5;
  params_self.m_position.m_character = 15;

  auto res_self_hover = lsp_handlers::hover(*workspace, 1, params_self);
  ASSERT_TRUE(res_self_hover.has_value());
  auto hover_self = res_self_hover.value().get<LSPSpec::Hover>();
  std::string text_self = hover_self.m_contents.m_value;
  EXPECT_NE(text_self.find("self"), std::string::npos);
  EXPECT_NE(text_self.find("enemy"), std::string::npos);
  EXPECT_NE(text_self.find("implicit self"), std::string::npos);

  // Definition on self on line 5, char 15
  auto res_self_def = lsp_handlers::go_to_definition(*workspace, 1, params_self);
  ASSERT_TRUE(res_self_def.has_value());
  auto locs_self = res_self_def.value().get<std::vector<LSPSpec::Location>>();
  ASSERT_EQ(locs_self.size(), 1);
  // Should jump to 'enemy' in defbehavior on line 4, char 31
  EXPECT_EQ(locs_self[0].m_range.m_start.m_line, 4);
  EXPECT_EQ(locs_self[0].m_range.m_start.m_character, 31);

  // Hover on root in (-> self root) on line 6, char 30
  LSPSpec::TextDocumentPositionParams params_root;
  params_root.m_textDocument.m_uri = file_uri;
  params_root.m_position.m_line = 6;
  params_root.m_position.m_character = 30;

  auto res_root_hover = lsp_handlers::hover(*workspace, 1, params_root);
  ASSERT_TRUE(res_root_hover.has_value());
  auto hover_root = res_root_hover.value().get<LSPSpec::Hover>();
  std::string text_root = hover_root.m_contents.m_value;
  EXPECT_NE(text_root.find("root"), std::string::npos);
  EXPECT_NE(text_root.find("Field"), std::string::npos);
  EXPECT_NE(text_root.find("enemy"), std::string::npos);
  EXPECT_NE(text_root.find("transform"), std::string::npos);

  // Definition on root on line 6, char 30
  auto res_root_def = lsp_handlers::go_to_definition(*workspace, 1, params_root);
  ASSERT_TRUE(res_root_def.has_value());
  auto locs_root = res_root_def.value().get<std::vector<LSPSpec::Location>>();
  ASSERT_EQ(locs_root.size(), 1);
  // Should jump to root field declaration inside deftype enemy (row 3 in the setup block)
  EXPECT_EQ(locs_root[0].m_range.m_start.m_line, 3);
}

// Test 2: self in defstate
TEST_F(LSPImplicitSelfAndLiteralTest, DefStateSelfAndFields) {
  std::string code = R"(
(deftype target (basic)
  ((state-time float)))

(defstate target-indax-flop-hit-ground (target)
  :enter (behavior ((arg0 symbol))
    (set-time! (-> self state-time))))
)";
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  // Hover on self on line 6, char 19
  LSPSpec::TextDocumentPositionParams params_self;
  params_self.m_textDocument.m_uri = file_uri;
  params_self.m_position.m_line = 6;
  params_self.m_position.m_character = 19;

  auto res_self_hover = lsp_handlers::hover(*workspace, 1, params_self);
  ASSERT_TRUE(res_self_hover.has_value());
  auto hover_self = res_self_hover.value().get<LSPSpec::Hover>();
  std::string text_self = hover_self.m_contents.m_value;
  EXPECT_NE(text_self.find("self"), std::string::npos);
  EXPECT_NE(text_self.find("target"), std::string::npos);

  // Hover on state-time on line 6, char 24
  LSPSpec::TextDocumentPositionParams params_time;
  params_time.m_textDocument.m_uri = file_uri;
  params_time.m_position.m_line = 6;
  params_time.m_position.m_character = 24;

  auto res_time_hover = lsp_handlers::hover(*workspace, 1, params_time);
  ASSERT_TRUE(res_time_hover.has_value());
  auto hover_time = res_time_hover.value().get<LSPSpec::Hover>();
  std::string text_time = hover_time.m_contents.m_value;
  EXPECT_NE(text_time.find("state-time"), std::string::npos);
  EXPECT_NE(text_time.find("Field"), std::string::npos);
  EXPECT_NE(text_time.find("target"), std::string::npos);
  EXPECT_NE(text_time.find("float"), std::string::npos);

  // Definition on state-time on line 6, char 24 (row 5 in setup block)
  auto res_time_def = lsp_handlers::go_to_definition(*workspace, 1, params_time);
  ASSERT_TRUE(res_time_def.has_value());
  auto locs_time = res_time_def.value().get<std::vector<LSPSpec::Location>>();
  ASSERT_EQ(locs_time.size(), 1);
  EXPECT_EQ(locs_time[0].m_range.m_start.m_line, 5);
}

// Test 3: nested behavior params plus implicit self
TEST_F(LSPImplicitSelfAndLiteralTest, DefStateNestedBehaviorParams) {
  std::string code = R"(
(deftype target (basic)
  ((state-time float)))

(defstate target-indax-flop-hit-ground (target)
  :enter (behavior ((arg0 symbol))
    (when arg0
      (set-time! (-> self state-time)))))
)";
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  // arg0 hover on line 6, char 10 -> param symbol
  LSPSpec::TextDocumentPositionParams params_arg0;
  params_arg0.m_textDocument.m_uri = file_uri;
  params_arg0.m_position.m_line = 6;
  params_arg0.m_position.m_character = 10; // pointing to arg0 in (when arg0 ...)

  auto res_arg0 = lsp_handlers::hover(*workspace, 1, params_arg0);
  ASSERT_TRUE(res_arg0.has_value());
  auto hover_arg0 = res_arg0.value().get<LSPSpec::Hover>();
  std::string text_arg0 = hover_arg0.m_contents.m_value;
  EXPECT_NE(text_arg0.find("arg0"), std::string::npos);
  EXPECT_NE(text_arg0.find("Parameter"), std::string::npos);
  EXPECT_NE(text_arg0.find("symbol"), std::string::npos);

  // self hover on line 7, char 21 -> target
  LSPSpec::TextDocumentPositionParams params_self;
  params_self.m_textDocument.m_uri = file_uri;
  params_self.m_position.m_line = 7;
  params_self.m_position.m_character = 21;

  auto res_self = lsp_handlers::hover(*workspace, 1, params_self);
  ASSERT_TRUE(res_self.has_value());
  auto hover_self = res_self.value().get<LSPSpec::Hover>();
  std::string text_self = hover_self.m_contents.m_value;
  EXPECT_NE(text_self.find("self"), std::string::npos);
  EXPECT_NE(text_self.find("target"), std::string::npos);
}

// Test 4: literal float let inference
TEST_F(LSPImplicitSelfAndLiteralTest, LiteralFloatInference) {
  std::string code = R"(
(defun test ()
  (let ((f26-0 0.0)
        (f30-0 819.2))
    (+ f26-0 f30-0)))
)";
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  // Hover f26-0 declaration on line 2, char 9
  LSPSpec::TextDocumentPositionParams params_decl;
  params_decl.m_textDocument.m_uri = file_uri;
  params_decl.m_position.m_line = 2;
  params_decl.m_position.m_character = 9;

  auto res_decl = lsp_handlers::hover(*workspace, 1, params_decl);
  ASSERT_TRUE(res_decl.has_value());
  auto hover_decl = res_decl.value().get<LSPSpec::Hover>();
  std::string text_decl = hover_decl.m_contents.m_value;
  EXPECT_NE(text_decl.find("f26-0"), std::string::npos);
  EXPECT_NE(text_decl.find("float"), std::string::npos);
  EXPECT_NE(text_decl.find("Local variable"), std::string::npos);

  // Hover f30-0 reference on line 4, char 13
  LSPSpec::TextDocumentPositionParams params_ref;
  params_ref.m_textDocument.m_uri = file_uri;
  params_ref.m_position.m_line = 4;
  params_ref.m_position.m_character = 13;

  auto res_ref = lsp_handlers::hover(*workspace, 1, params_ref);
  ASSERT_TRUE(res_ref.has_value());
  auto hover_ref = res_ref.value().get<LSPSpec::Hover>();
  std::string text_ref = hover_ref.m_contents.m_value;
  EXPECT_NE(text_ref.find("f30-0"), std::string::npos);
  EXPECT_NE(text_ref.find("float"), std::string::npos);
}

// Test 5: integer literal let inference
TEST_F(LSPImplicitSelfAndLiteralTest, LiteralIntInference) {
  std::string code = R"(
(defun test ()
  (let ((count 0))
    count))
)";
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  // Hover count reference on line 3, char 4
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 3;
  params.m_position.m_character = 4;

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("count"), std::string::npos);
  EXPECT_NE(text.find("int"), std::string::npos);
}

// Test 6: boolean literal let inference
TEST_F(LSPImplicitSelfAndLiteralTest, LiteralBoolInference) {
  std::string code = R"(
(defun test ()
  (let ((enabled #f)
        (other #t))
    enabled))
)";
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  // Hover enabled reference on line 4, char 4
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 4;
  params.m_position.m_character = 4;

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("enabled"), std::string::npos);
  // Let's accept bool or symbol or symbol/bool
  EXPECT_TRUE(text.find("bool") != std::string::npos || text.find("symbol") != std::string::npos);
}

// Test 7: string literal let inference
TEST_F(LSPImplicitSelfAndLiteralTest, LiteralStringInference) {
  std::string code = R"(
(defun test ()
  (let ((name "hello"))
    name))
)";
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  // Hover name reference on line 3, char 4
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 3;
  params.m_position.m_character = 4;

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("name"), std::string::npos);
  EXPECT_NE(text.find("string"), std::string::npos);
}

// Test 8: literal inference should not override explicit cast
TEST_F(LSPImplicitSelfAndLiteralTest, CastPrecedence) {
  std::string code = R"(
(defun test ()
  (let ((x (the-as uint 0)))
    x))
)";
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  // Hover x reference on line 3, char 4
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 3;
  params.m_position.m_character = 4;

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("x"), std::string::npos);
  EXPECT_NE(text.find("uint"), std::string::npos);
  EXPECT_EQ(text.find(": int"), std::string::npos);
}

// Test 9: regression for function return inference
TEST_F(LSPImplicitSelfAndLiteralTest, FunctionReturnRegression) {
  std::string code = R"(
(defun test-func ((name-str string))
  (let ((proc (process-by-name name-str *active-pool*)))
    proc))
)";
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  // Hover proc reference on line 3, char 4
  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 3;
  params.m_position.m_character = 4;

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("proc"), std::string::npos);
  EXPECT_NE(text.find("process"), std::string::npos);
}

// Test 10: top-level define with new
TEST_F(LSPImplicitSelfAndLiteralTest, TopLevelDefineWithNew) {
  std::string code = R"(
(define *my-test-active-pool* (new 'global 'process-tree "active-pool"))
(defun use-active ()
  *my-test-active-pool*)
)";
  workspace->get_compiler(GameVersion::Jak2)->run_front_end_on_string(code);
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 3;
  params.m_position.m_character = 4; // points to *my-test-active-pool* inside use-active

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("*my-test-active-pool*"), std::string::npos);
  EXPECT_NE(text.find("process-tree"), std::string::npos);

  auto res_def = lsp_handlers::go_to_definition(*workspace, 1, params);
  ASSERT_TRUE(res_def.has_value());
  auto locations = res_def.value().get<std::vector<LSPSpec::Location>>();
  ASSERT_EQ(locations.size(), 1);
  EXPECT_EQ(locations[0].m_range.m_start.m_line, 1);
}

// Test 11: nested define inside change-parent
TEST_F(LSPImplicitSelfAndLiteralTest, NestedDefineInsideChangeParent) {
  std::string code = R"(
(define-extern change-parent (function process-tree process-tree process-tree))
(define *my-test-active-pool* (new 'global 'process-tree "active-pool"))
(change-parent
  (define *my-test-display-pool* (new 'global 'process-tree "display-pool"))
  *my-test-active-pool*)
)";
  workspace->get_compiler(GameVersion::Jak2)->run_front_end_on_string(code);
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  // Hover *my-test-display-pool* on line 4, char 15
  LSPSpec::TextDocumentPositionParams params_disp;
  params_disp.m_textDocument.m_uri = file_uri;
  params_disp.m_position.m_line = 4;
  params_disp.m_position.m_character = 15;

  auto res = lsp_handlers::hover(*workspace, 1, params_disp);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("*my-test-display-pool*"), std::string::npos);
  EXPECT_NE(text.find("process-tree"), std::string::npos);

  // Hover *my-test-active-pool* on line 5, char 5
  LSPSpec::TextDocumentPositionParams params_act;
  params_act.m_textDocument.m_uri = file_uri;
  params_act.m_position.m_line = 5;
  params_act.m_position.m_character = 5;

  auto res2 = lsp_handlers::hover(*workspace, 1, params_act);
  ASSERT_TRUE(res2.has_value());
  auto hover2 = res2.value().get<LSPSpec::Hover>();
  EXPECT_NE(hover2.m_contents.m_value.find("*my-test-active-pool*"), std::string::npos);
  EXPECT_NE(hover2.m_contents.m_value.find("process-tree"), std::string::npos);
}

// Test 12: define with the-as
TEST_F(LSPImplicitSelfAndLiteralTest, DefineWithTheAs) {
  std::string code = R"(
(define *my-test-mp-puppet* (the-as target #f))
(defun use-puppet ()
  *my-test-mp-puppet*)
)";
  workspace->get_compiler(GameVersion::Jak2)->run_front_end_on_string(code);
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 3;
  params.m_position.m_character = 4;

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("*my-test-mp-puppet*"), std::string::npos);
  EXPECT_NE(text.find("target"), std::string::npos);

  auto res_def = lsp_handlers::go_to_definition(*workspace, 1, params);
  ASSERT_TRUE(res_def.has_value());
  auto locations = res_def.value().get<std::vector<LSPSpec::Location>>();
  ASSERT_EQ(locations.size(), 1);
  EXPECT_EQ(locations[0].m_range.m_start.m_line, 1);
}

// Test 13: define with new static
TEST_F(LSPImplicitSelfAndLiteralTest, DefineWithNewStatic) {
  std::string code = R"(
(deftype mp-target-ghost-record (basic) ())
(define-extern static symbol)
(define *my-test-mp-puppet-ghost* (new 'static 'mp-target-ghost-record))
(defun use-ghost ()
  *my-test-mp-puppet-ghost*)
)";
  workspace->get_compiler(GameVersion::Jak2)->run_front_end_on_string(code);
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 5;
  params.m_position.m_character = 4;

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("*my-test-mp-puppet-ghost*"), std::string::npos);
  EXPECT_NE(text.find("mp-target-ghost-record"), std::string::npos);

  auto res_def = lsp_handlers::go_to_definition(*workspace, 1, params);
  ASSERT_TRUE(res_def.has_value());
  auto locations = res_def.value().get<std::vector<LSPSpec::Location>>();
  ASSERT_EQ(locations.size(), 1);
  EXPECT_EQ(locations[0].m_range.m_start.m_line, 3);
}

// Test 14: regression define-perm
TEST_F(LSPImplicitSelfAndLiteralTest, RegressionDefinePerm) {
  std::string code = R"(
(define-perm *my-test-target* target #f)
(defun use-target ()
  *my-test-target*)
)";
  workspace->get_compiler(GameVersion::Jak2)->run_front_end_on_string(code);
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 3;
  params.m_position.m_character = 4;

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("*my-test-target*"), std::string::npos);
  EXPECT_NE(text.find("target"), std::string::npos);

  auto res_def = lsp_handlers::go_to_definition(*workspace, 1, params);
  ASSERT_TRUE(res_def.has_value());
  auto locations = res_def.value().get<std::vector<LSPSpec::Location>>();
  ASSERT_EQ(locations.size(), 1);
  EXPECT_EQ(locations[0].m_range.m_start.m_line, 1);
}

// Test 15: regression define-extern
TEST_F(LSPImplicitSelfAndLiteralTest, RegressionDefineExtern) {
  std::string code = R"(
(define-extern *my-test-active-pool* process-tree)
(defun use-active ()
  *my-test-active-pool*)
)";
  workspace->get_compiler(GameVersion::Jak2)->run_front_end_on_string(code);
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 3;
  params.m_position.m_character = 4;

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("*my-test-active-pool*"), std::string::npos);
  EXPECT_NE(text.find("process-tree"), std::string::npos);
}

class LSPBugFixesTest : public LSPHoverDefinitionTest {
 protected:
  void SetUp() override {
    LSPHoverDefinitionTest::SetUp();
  }
};

// Bug 1: local initialized from zero-arg function call
TEST_F(LSPBugFixesTest, ZeroArgFunctionInference) {
  std::string code = R"(
(define-extern pc-multi-get-ticks (function uint64))

(defun test-zero-arg ()
  (let ((current-ticks (pc-multi-get-ticks)))
    current-ticks))
)";
  workspace->get_compiler(GameVersion::Jak2)->run_front_end_on_string(code);
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  // reference to current-ticks on line 6
  params.m_position.m_line = 5;
  params.m_position.m_character = 5;

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("current-ticks"), std::string::npos);
  EXPECT_NE(text.find("uint64"), std::string::npos);
  EXPECT_NE(text.find("Local variable"), std::string::npos);
}

// Bug 2: 'the' cast form
TEST_F(LSPBugFixesTest, TheCastInference) {
  std::string code = R"(
(define-extern *multi-sync-tps* uint64)

(defun test-the-cast ()
  (let ((interval (the uint64 (/ 1000 *multi-sync-tps*))))
    interval))
)";
  workspace->get_compiler(GameVersion::Jak2)->run_front_end_on_string(code);
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  // binding 'interval' on line 5
  params.m_position.m_line = 4;
  params.m_position.m_character = 10;

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  auto hover = res.value().get<LSPSpec::Hover>();
  std::string text = hover.m_contents.m_value;
  EXPECT_NE(text.find("interval"), std::string::npos);
  EXPECT_NE(text.find("uint64"), std::string::npos);
}

// Bug 3: let* sequential bindings and shadowing
TEST_F(LSPBugFixesTest, LetStarShadowing) {
  std::string code = R"(
(defun test-let-star ()
  (let* ((a 1)
         (a (the int a))
         (b a))
    b))
)";
  workspace->get_compiler(GameVersion::Jak2)->run_front_end_on_string(code);
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  // Hover 'a' in (the int a) - should be the first 'a'
  {
    LSPSpec::TextDocumentPositionParams params;
    params.m_textDocument.m_uri = file_uri;
    params.m_position.m_line = 3;
    params.m_position.m_character = 21; // inside (the int a)
    auto res = lsp_handlers::hover(*workspace, 1, params);
    ASSERT_TRUE(res.has_value());
    EXPECT_NE(res.value().get<LSPSpec::Hover>().m_contents.m_value.find("int"), std::string::npos);
    
    // Definition should go to first 'a' (line 3, zero-indexed)
    auto res_def = lsp_handlers::go_to_definition(*workspace, 1, params);
    ASSERT_TRUE(res_def.has_value());
    EXPECT_EQ(res_def.value().get<std::vector<LSPSpec::Location>>()[0].m_range.m_start.m_line, 2);
  }

  // Hover 'a' in (b a) - should be the SECOND 'a' (the shadowed one)
  {
    LSPSpec::TextDocumentPositionParams params;
    params.m_textDocument.m_uri = file_uri;
    params.m_position.m_line = 4;
    params.m_position.m_character = 12;
    auto res = lsp_handlers::hover(*workspace, 1, params);
    ASSERT_TRUE(res.has_value());
    EXPECT_NE(res.value().get<LSPSpec::Hover>().m_contents.m_value.find("int"), std::string::npos);
    
    // Definition should go to second 'a' (line 4)
    auto res_def = lsp_handlers::go_to_definition(*workspace, 1, params);
    ASSERT_TRUE(res_def.has_value());
    EXPECT_EQ(res_def.value().get<std::vector<LSPSpec::Location>>()[0].m_range.m_start.m_line, 3);
  }

  // Hover 'b' in body
  {
    LSPSpec::TextDocumentPositionParams params;
    params.m_textDocument.m_uri = file_uri;
    params.m_position.m_line = 5;
    params.m_position.m_character = 4;
    auto res = lsp_handlers::hover(*workspace, 1, params);
    ASSERT_TRUE(res.has_value());
    EXPECT_NE(res.value().get<LSPSpec::Hover>().m_contents.m_value.find("int"), std::string::npos);
  }
}

// Bug 4: defbehavior precedence over define-extern (tested via defun to avoid compiler macro complexity in isolated env)
TEST_F(LSPBugFixesTest, BehaviorPrecedence) {
  std::string code = R"(
(define-extern my-func (function int int))

(defun my-func ((arg0 int))
  0)

(defun call-my-func ()
  (my-func 1))
)";
  // Extern first, then definition. Both have exact matching signature.
  workspace->get_compiler(GameVersion::Jak2)->run_front_end_on_string(R"(
(define-extern my-func (function int int))
(defun my-func ((arg0 int)) 0)
)");

  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  // call site on line 8
  params.m_position.m_line = 7;
  params.m_position.m_character = 4;

  // Hover should prefer the concrete definition (FUNCTION) over the extern (FWD_DECLARED_SYM)
  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  std::string text = res.value().get<LSPSpec::Hover>().m_contents.m_value;
  EXPECT_NE(text.find("my-func"), std::string::npos);
  
  // It should resolve to the Function info
  EXPECT_NE(text.find("Function"), std::string::npos);
}

// Regression: the-as still works
TEST_F(LSPBugFixesTest, TheAsRegression) {
  std::string code = R"(
(defun test-the-as ()
  (let ((val (the-as uint64 0)))
    val))
)";
  workspace->get_compiler(GameVersion::Jak2)->run_front_end_on_string(code);
  WorkspaceOGFile file(file_uri, code, GameVersion::Jak2);
  workspace->inject_tracked_og_file(file_uri, file);

  LSPSpec::TextDocumentPositionParams params;
  params.m_textDocument.m_uri = file_uri;
  params.m_position.m_line = 3;
  params.m_position.m_character = 5;

  auto res = lsp_handlers::hover(*workspace, 1, params);
  ASSERT_TRUE(res.has_value());
  EXPECT_NE(res.value().get<LSPSpec::Hover>().m_contents.m_value.find("uint64"), std::string::npos);
}




