#include "gtest/gtest.h"

#include "lsp/protocol/hover_formatting.h"

namespace {

std::string get_markdown(const lsp_hover::HoverInfo& info) {
  return lsp_hover::format_hover_markdown(info);
}

TEST(LSPHoverFormattingTest, ParameterHover) {
  lsp_hover::HoverInfo info;
  info.kind = lsp_hover::HoverKind::Parameter;
  info.name = "buttons";
  info.type = "uint";

  const auto markdown = get_markdown(info);

  EXPECT_NE(markdown.find("```goal\nbuttons: uint\n```"), std::string::npos);
  EXPECT_NE(markdown.find("**Parameter**"), std::string::npos);
  EXPECT_EQ(markdown.find("(parameter)"), std::string::npos);
}

TEST(LSPHoverFormattingTest, LocalHover) {
  lsp_hover::HoverInfo info;
  info.kind = lsp_hover::HoverKind::Local;
  info.name = "proc";
  info.type = "process";

  const auto markdown = get_markdown(info);

  EXPECT_NE(markdown.find("```goal\nproc: process\n```"), std::string::npos);
  EXPECT_NE(markdown.find("**Local variable**"), std::string::npos);
}

TEST(LSPHoverFormattingTest, FieldHover) {
  lsp_hover::HoverInfo info;
  info.kind = lsp_hover::HoverKind::Field;
  info.name = "button0-rel";
  info.type = "pad-buttons";
  info.owner_type = "cpad-history";

  const auto markdown = get_markdown(info);

  EXPECT_NE(markdown.find("```goal\nbutton0-rel: pad-buttons\n```"), std::string::npos);
  EXPECT_NE(markdown.find("**Field** of `cpad-history`"), std::string::npos);
}

TEST(LSPHoverFormattingTest, FunctionHover) {
  lsp_hover::HoverInfo info;
  info.kind = lsp_hover::HoverKind::Function;
  info.name = "mp-puppet-update-pad";
  info.return_type = "none";
  info.docstring =
      "Manually update a cpad-info structure with raw input data, shifting history and calculating derived fields.";
  info.params = {
      {"pad", "cpad-info", ""},
      {"buttons", "uint", ""},
      {"lx", "uint", ""},
      {"ly", "uint", ""},
      {"rx", "uint", ""},
      {"ry", "uint", ""},
  };

  const auto markdown = get_markdown(info);

  EXPECT_NE(markdown.find(
                "```goal\n(defun mp-puppet-update-pad ((pad cpad-info) (buttons uint) (lx uint) (ly uint) (rx uint) (ry uint)) -> none)\n```"),
            std::string::npos);
  EXPECT_NE(markdown.find("**Function**"), std::string::npos);
  EXPECT_NE(markdown.find("Manually update a cpad-info structure with raw input data, shifting history and calculating derived fields."),
            std::string::npos);
  EXPECT_NE(markdown.find("**Parameters**"), std::string::npos);
  EXPECT_NE(markdown.find("* `pad`: `cpad-info`"), std::string::npos);
  EXPECT_NE(markdown.find("**Returns** `none`"), std::string::npos);
}

TEST(LSPHoverFormattingTest, MacroHover) {
  lsp_hover::HoverInfo info;
  info.kind = lsp_hover::HoverKind::Macro;
  info.name = "dotimes";
  info.docstring = "Loop like for ...";

  const auto markdown = get_markdown(info);

  EXPECT_NE(markdown.find("```goal\n(dotimes ...)\n```"), std::string::npos);
  EXPECT_NE(markdown.find("**Macro**"), std::string::npos);
  EXPECT_NE(markdown.find("Loop like for ..."), std::string::npos);
}

TEST(LSPHoverFormattingTest, TypeHover) {
  lsp_hover::HoverInfo info;
  info.kind = lsp_hover::HoverKind::Type;
  info.name = "cpad-info";
  info.parent_type = "basic";

  const auto markdown = get_markdown(info);

  EXPECT_NE(markdown.find("```goal\n(deftype cpad-info ...)\n```"), std::string::npos);
  EXPECT_NE(markdown.find("**Type**"), std::string::npos);
  EXPECT_NE(markdown.find("**Extends** `basic`"), std::string::npos);
}

TEST(LSPHoverFormattingTest, GlobalHover) {
  lsp_hover::HoverInfo info;
  info.kind = lsp_hover::HoverKind::Global;
  info.name = "*active-pool*";
  info.type = "process-tree";

  const auto markdown = get_markdown(info);

  EXPECT_NE(markdown.find("```goal\n*active-pool*: process-tree\n```"), std::string::npos);
  EXPECT_NE(markdown.find("**Global variable**"), std::string::npos);
}

TEST(LSPHoverFormattingTest, MissingTypeFallback) {
  lsp_hover::HoverInfo info;
  info.kind = lsp_hover::HoverKind::Local;
  info.name = "x";

  const auto markdown = get_markdown(info);

  EXPECT_TRUE(markdown.find("```goal\nx: unknown\n```") != std::string::npos ||
              markdown.find("```goal\nx\n```") != std::string::npos);
  EXPECT_NE(markdown.find("**Local variable**"), std::string::npos);
}

TEST(LSPHoverFormattingTest, EscapesMarkdownInProse) {
  lsp_hover::HoverInfo info;
  info.kind = lsp_hover::HoverKind::Global;
  info.name = "*active-pool*";
  info.type = "process-tree";
  info.docstring = "Escapes *markdown* _here_ <safe> |pipe|.";

  const auto markdown = get_markdown(info);

  EXPECT_NE(markdown.find("```goal\n*active-pool*: process-tree\n```"), std::string::npos);
  EXPECT_NE(markdown.find("Escapes \\*markdown\\* \\_here\\_ \\<safe\\> \\|pipe\\|."),
            std::string::npos);
  EXPECT_EQ(markdown.find("Escapes *markdown* _here_ <safe> |pipe|."), std::string::npos);
}

}  // namespace
