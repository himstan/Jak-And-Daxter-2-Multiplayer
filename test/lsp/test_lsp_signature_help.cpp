#include "gtest/gtest.h"

#include <memory>
#include <string>

#include "goalc/compiler/Compiler.h"
#include "lsp/handlers/initialize.h"
#include "lsp/handlers/text_document/signature_help.h"
#include "lsp/protocol/common_types.h"
#include "lsp/protocol/signature_help.h"
#include "lsp/state/workspace.h"

class LSPSignatureHelpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    compiler = std::make_unique<Compiler>(GameVersion::Jak2, emitter::InstructionSet::X86);
    workspace = std::make_unique<Workspace>();
    workspace->set_initialized(true);
    workspace->inject_compiler(GameVersion::Jak2, std::move(compiler));
    file_uri = "file:///goal_src/jak2/multiplayer/core/test-signature-help.gc";
  }

  void load_code(const std::string& code_with_cursor) {
    const auto marker_pos = code_with_cursor.find('|');
    ASSERT_NE(marker_pos, std::string::npos);

    current_code = code_with_cursor;
    current_code.erase(marker_pos, 1);
    current_cursor = offset_to_position(marker_pos, code_with_cursor);

    WorkspaceOGFile file(file_uri, current_code, GameVersion::Jak2);
    workspace->inject_tracked_og_file(file_uri, file);
  }

  LSPSpec::SignatureHelp signature_help() {
    LSPSpec::SignatureHelpParams params;
    params.m_textDocument.m_uri = file_uri;
    params.m_position = current_cursor;

    auto result = lsp_handlers::signature_help(*workspace, 1, json(params));
    if (!result.has_value() || result.value().is_null()) {
      return {};
    }
    return result.value().get<LSPSpec::SignatureHelp>();
  }

  static LSPSpec::Position offset_to_position(size_t offset, const std::string& text) {
    LSPSpec::Position pos{};
    pos.m_line = 0;
    pos.m_character = 0;

    for (size_t i = 0; i < offset; ++i) {
      if (text[i] == '\n') {
        pos.m_line++;
        pos.m_character = 0;
      } else {
        pos.m_character++;
      }
    }
    return pos;
  }

  void expect_label_contains(const LSPSpec::SignatureHelp& help, const std::string& needle) {
    ASSERT_FALSE(help.m_signatures.empty());
    EXPECT_NE(help.m_signatures.front().m_label.find(needle), std::string::npos)
        << help.m_signatures.front().m_label;
  }

  std::unique_ptr<Workspace> workspace;
  std::unique_ptr<Compiler> compiler;
  std::string file_uri;
  std::string current_code;
  LSPSpec::Position current_cursor{};
};

TEST_F(LSPSignatureHelpTest, InitializeAdvertisesSignatureHelpProvider) {
  auto result = lsp_handlers::initialize(*workspace, 1, json::object());
  ASSERT_TRUE(result.has_value());
  auto caps = result.value().at("capabilities");
  ASSERT_TRUE(caps.contains("signatureHelpProvider"));
  EXPECT_EQ(caps.at("signatureHelpProvider").at("triggerCharacters"),
            json::array({" ", "(", ","}));
  EXPECT_EQ(caps.at("signatureHelpProvider").at("retriggerCharacters"), json::array({" "}));
}

TEST_F(LSPSignatureHelpTest, FunctionFirstParameter) {
  load_code(R"(
(define-extern process-by-name (function string process-tree process))

(defun test ((name-str string))
  (process-by-name |))
)");

  auto help = signature_help();
  ASSERT_EQ(help.m_signatures.size(), 1);
  expect_label_contains(help, "process-by-name");
  expect_label_contains(help, "string");
  expect_label_contains(help, "process-tree");
  expect_label_contains(help, "process");
  EXPECT_EQ(help.m_activeSignature, 0);
  ASSERT_TRUE(help.m_activeParameter.has_value());
  EXPECT_EQ(help.m_activeParameter.value(), 0);
}

TEST_F(LSPSignatureHelpTest, FunctionSecondParameter) {
  load_code(R"(
(define-extern process-by-name (function string process-tree process))
(define-extern *active-pool* process-tree)

(defun test ((name-str string))
  (process-by-name name-str |))
)");

  auto help = signature_help();
  ASSERT_EQ(help.m_signatures.size(), 1);
  expect_label_contains(help, "process-by-name");
  EXPECT_EQ(help.m_activeSignature, 0);
  ASSERT_TRUE(help.m_activeParameter.has_value());
  EXPECT_EQ(help.m_activeParameter.value(), 1);
}

TEST_F(LSPSignatureHelpTest, CompletedArgumentClampsToLastParameter) {
  load_code(R"(
(define-extern process-by-name (function string process-tree process))
(define-extern *active-pool* process-tree)

(defun test ((name-str string))
  (process-by-name name-str *active-pool*|))
)");

  auto help = signature_help();
  ASSERT_EQ(help.m_signatures.size(), 1);
  ASSERT_TRUE(help.m_activeParameter.has_value());
  EXPECT_EQ(help.m_activeParameter.value(), 1);
}

TEST_F(LSPSignatureHelpTest, ZeroArgumentFunction) {
  load_code(R"(
(define-extern pc-multi-get-ticks (function uint64))

(defun test ()
  (pc-multi-get-ticks |))
)");

  auto help = signature_help();
  ASSERT_EQ(help.m_signatures.size(), 1);
  expect_label_contains(help, "pc-multi-get-ticks");
  expect_label_contains(help, "uint64");
  EXPECT_TRUE(help.m_signatures.front().m_parameters.empty());
}

TEST_F(LSPSignatureHelpTest, NestedOuterCall) {
  load_code(R"(
(define-extern inner (function int string))
(define-extern outer (function string int none))

(defun test ((x int))
  (outer (inner x) |))
)");

  auto help = signature_help();
  ASSERT_EQ(help.m_signatures.size(), 1);
  expect_label_contains(help, "outer");
  ASSERT_TRUE(help.m_activeParameter.has_value());
  EXPECT_EQ(help.m_activeParameter.value(), 1);
}

TEST_F(LSPSignatureHelpTest, NestedInnerCall) {
  load_code(R"(
(define-extern inner (function int string))
(define-extern outer (function string int none))

(defun test ((x int))
  (outer (inner |) 123))
)");

  auto help = signature_help();
  ASSERT_EQ(help.m_signatures.size(), 1);
  expect_label_contains(help, "inner");
  ASSERT_TRUE(help.m_activeParameter.has_value());
  EXPECT_EQ(help.m_activeParameter.value(), 0);
}

TEST_F(LSPSignatureHelpTest, BehaviorExternDoesNotCrash) {
  load_code(R"(
(deftype target (basic) ())

(define-extern target-gun-fire-blue (function (pointer process) :behavior target))

(defun test ()
  (target-gun-fire-blue |))
)");

  auto help = signature_help();
  ASSERT_EQ(help.m_signatures.size(), 1);
  expect_label_contains(help, "target-gun-fire-blue");
  expect_label_contains(help, "behavior");
  expect_label_contains(help, "target");
}

TEST_F(LSPSignatureHelpTest, DefbehaviorBeatsExtern) {
  load_code(R"(
(deftype target (basic) ())

(defbehavior target-gun-fire-blue target ()
  (none))

(define-extern target-gun-fire-blue (function (pointer process) :behavior target))

(defun test ()
  (target-gun-fire-blue |))
)");

  auto help = signature_help();
  ASSERT_EQ(help.m_signatures.size(), 1);
  expect_label_contains(help, "target-gun-fire-blue");
  expect_label_contains(help, "behavior");
  expect_label_contains(help, "target");
}

TEST_F(LSPSignatureHelpTest, MacroFallbackIsEmptyOrDotimesSignature) {
  load_code(R"(
(defun test ()
  (dotimes |))
)");

  auto help = signature_help();
  if (!help.m_signatures.empty()) {
    expect_label_contains(help, "dotimes");
  }
}

TEST_F(LSPSignatureHelpTest, UnknownCallFallbackIsEmpty) {
  load_code(R"(
(defun test ()
  (unknown-call |))
)");

  auto help = signature_help();
  EXPECT_TRUE(help.m_signatures.empty());
}
