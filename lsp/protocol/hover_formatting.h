#pragma once

#include <string>
#include <vector>

#include "lsp/protocol/hover.h"

namespace lsp_hover {

enum class HoverKind {
  Function,
  Method,
  Macro,
  Global,
  Parameter,
  Local,
  Field,
  Type,
  Constant,
  EnumValue,
  Unknown,
};

struct HoverParam {
  std::string name;
  std::string type;
  std::string doc;
};

struct HoverInfo {
  HoverKind kind = HoverKind::Unknown;
  std::string name;
  std::string type;
  std::string owner_type;
  std::string parent_type;
  std::string return_type;
  std::vector<HoverParam> params;
  std::vector<HoverParam> fields;
  std::string docstring;
  std::string signature;
};

std::string format_hover_markdown(const HoverInfo& info);
LSPSpec::Hover make_markdown_hover(const HoverInfo& info);

}  // namespace lsp_hover
