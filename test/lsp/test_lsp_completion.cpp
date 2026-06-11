#include "gtest/gtest.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "goalc/compiler/Compiler.h"
#include "lsp/handlers/text_document/completion.h"
#include "lsp/protocol/completion.h"
#include "lsp/state/workspace.h"

class LSPFieldCompletionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    compiler = std::make_unique<Compiler>(GameVersion::Jak2, emitter::InstructionSet::X86);
    compiler->run_front_end_on_string(R"(
(deftype pad-buttons (basic)
  ((triangle uint)
   (circle uint)
   (cross uint)
   (square uint)))

(deftype cpad-history (basic)
  ((button0-rel pad-buttons)
   (button0-abs pad-buttons)))

(deftype cpad-info (basic)
  ((history cpad-history)
   (buttons pad-buttons)))

(define-extern process-by-name (function string cpad-info cpad-info))
(define-extern *pad* cpad-info)
)");

    workspace = std::make_unique<Workspace>();
    workspace->set_initialized(true);
    workspace->inject_compiler(GameVersion::Jak2, std::move(compiler));
    file_uri = "file:///goal_src/jak2/multiplayer/core/test-completion.gc";
  }

  void load_code(const std::string& code_with_cursor) {
    auto marker_pos = code_with_cursor.find('|');
    ASSERT_NE(marker_pos, std::string::npos);

    current_code = code_with_cursor;
    current_code.erase(marker_pos, 1);
    current_cursor = offset_to_position(marker_pos, code_with_cursor);

    WorkspaceOGFile file(file_uri, current_code, GameVersion::Jak2);
    workspace->inject_tracked_og_file(file_uri, file);
  }

  LSPSpec::CompletionList completion_list() {
    LSPSpec::CompletionParams params;
    params.textDocument.m_uri = file_uri;
    params.position = current_cursor;

    auto res = lsp_handlers::get_completions(*workspace, 1, params);
    if (!res.has_value()) {
      EXPECT_TRUE(false) << "completion handler returned no result";
      return LSPSpec::CompletionList{false, {}};
    }
    return res.value().get<LSPSpec::CompletionList>();
  }

  const LSPSpec::CompletionItem* find_item(const LSPSpec::CompletionList& list,
                                           const std::string& label) const {
    for (const auto& item : list.items) {
      if (item.label == label) {
        return &item;
      }
    }
    return nullptr;
  }

  void expect_labels_present(const LSPSpec::CompletionList& list,
                             const std::vector<std::string>& labels) const {
    for (const auto& label : labels) {
      EXPECT_NE(find_item(list, label), nullptr) << "missing completion item: " << label;
    }
  }

  void expect_labels_absent(const LSPSpec::CompletionList& list,
                            const std::vector<std::string>& labels) const {
    for (const auto& label : labels) {
      EXPECT_EQ(find_item(list, label), nullptr) << "unexpected completion item: " << label;
    }
  }

  LSPSpec::Position position_of_substring(const std::string& needle) const {
    auto offset = current_code.find(needle);
    EXPECT_NE(offset, std::string::npos);
    return offset_to_position(offset, current_code);
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

  std::unique_ptr<Workspace> workspace;
  std::unique_ptr<Compiler> compiler;
  std::string file_uri;
  std::string current_code;
  LSPSpec::Position current_cursor{};
};

TEST_F(LSPFieldCompletionTest, ParameterReceiverFields) {
  load_code(R"(
(defun test-pad ((pad cpad-info))
  (-> pad |))
)");

  auto list = completion_list();
  expect_labels_present(list, {"history", "buttons"});
  expect_labels_absent(list, {"process-by-name", "pad-buttons"});
}

TEST_F(LSPFieldCompletionTest, PrefixReplacement) {
  load_code(R"(
(defun test-pad ((pad cpad-info))
  (-> pad hist|))
)");

  auto list = completion_list();
  auto* history = find_item(list, "history");
  ASSERT_NE(history, nullptr);
  ASSERT_TRUE(history->textEdit.has_value());
  EXPECT_EQ(history->textEdit->newText, "history");
  EXPECT_EQ(history->kind.value(), LSPSpec::CompletionItemKind::Field);
  EXPECT_EQ(history->detail.value(), "cpad-history");
  EXPECT_EQ(history->documentation.value(), "field of cpad-info");

  auto prefix_start = position_of_substring("hist");
  EXPECT_EQ(history->textEdit->range.m_start.m_line, prefix_start.m_line);
  EXPECT_EQ(history->textEdit->range.m_start.m_character, prefix_start.m_character);
  EXPECT_EQ(history->textEdit->range.m_end.m_line, current_cursor.m_line);
  EXPECT_EQ(history->textEdit->range.m_end.m_character, current_cursor.m_character);
}

TEST_F(LSPFieldCompletionTest, ChainedFieldCompletion) {
  load_code(R"(
(defun test-pad ((pad cpad-info))
  (-> pad history |))
)");

  auto list = completion_list();
  expect_labels_present(list, {"button0-rel", "button0-abs"});
  expect_labels_absent(list, {"history", "buttons", "process-by-name"});
}

TEST_F(LSPFieldCompletionTest, DeeperChainedFieldCompletion) {
  load_code(R"(
(defun test-pad ((pad cpad-info))
  (-> pad history button0-rel |))
)");

  auto list = completion_list();
  expect_labels_present(list, {"triangle", "circle", "cross", "square"});
}

TEST_F(LSPFieldCompletionTest, GlobalReceiverFieldCompletion) {
  load_code(R"(
(define-extern *pad* cpad-info)

(defun test ()
  (-> *pad* |))
)");

  auto list = completion_list();
  expect_labels_present(list, {"history", "buttons"});
}

TEST_F(LSPFieldCompletionTest, LocalReceiverFieldCompletion) {
  load_code(R"(
(defun test-pad ((pad cpad-info))
  (let ((history (-> pad history)))
    (-> history |)))
)");

  auto list = completion_list();
  expect_labels_present(list, {"button0-rel", "button0-abs"});
}

TEST_F(LSPFieldCompletionTest, AmpersandArrowSupport) {
  load_code(R"(
(defun test-pad ((pad cpad-info))
  (&-> pad |))
)");

  auto list = completion_list();
  expect_labels_present(list, {"history", "buttons"});
}

TEST_F(LSPFieldCompletionTest, UnknownReceiverFallbackIsEmpty) {
  load_code(R"(
(defun test ((x object))
  (-> x |))
)");

  auto list = completion_list();
  EXPECT_TRUE(list.items.empty());
}

TEST_F(LSPFieldCompletionTest, NormalCompletionOutsideFieldAccessUnchanged) {
  load_code(R"(
(defun test-pad ((pad cpad-info))
  (pro|))
)");

  auto list = completion_list();
  expect_labels_present(list, {"process-by-name"});
  expect_labels_absent(list, {"history", "buttons", "triangle"});
}

TEST_F(LSPFieldCompletionTest, FieldMetadataIncludesKindTypeAndDocs) {
  load_code(R"(
(defun test-pad ((pad cpad-info))
  (-> pad history |))
)");

  auto list = completion_list();
  auto* button0_rel = find_item(list, "button0-rel");
  ASSERT_NE(button0_rel, nullptr);
  EXPECT_EQ(button0_rel->kind.value(), LSPSpec::CompletionItemKind::Field);
  EXPECT_EQ(button0_rel->detail.value(), "pad-buttons");
  EXPECT_EQ(button0_rel->documentation.value(), "field of cpad-history");
}
