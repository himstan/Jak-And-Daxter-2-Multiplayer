#pragma once

#include "common_types.h"

namespace LSPSpec {

struct ParameterInformation {
  std::string m_label;
  std::optional<MarkupContent> m_documentation;
};
void to_json(json& j, const ParameterInformation& obj);
void from_json(const json& j, ParameterInformation& obj);

struct SignatureInformation {
  std::string m_label;
  std::optional<MarkupContent> m_documentation;
  std::vector<ParameterInformation> m_parameters;
  std::optional<uint32_t> m_activeParameter;
};
void to_json(json& j, const SignatureInformation& obj);
void from_json(const json& j, SignatureInformation& obj);

struct SignatureHelp {
  std::vector<SignatureInformation> m_signatures;
  uint32_t m_activeSignature = 0;
  std::optional<uint32_t> m_activeParameter;
};
void to_json(json& j, const SignatureHelp& obj);
void from_json(const json& j, SignatureHelp& obj);

struct SignatureHelpParams : public TextDocumentPositionParams {};
void to_json(json& j, const SignatureHelpParams& obj);
void from_json(const json& j, SignatureHelpParams& obj);

}  // namespace LSPSpec
