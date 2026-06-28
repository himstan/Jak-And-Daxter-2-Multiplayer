#include "signature_help.h"

void LSPSpec::to_json(json& j, const ParameterInformation& obj) {
  j = json{{"label", obj.m_label}};
  if (obj.m_documentation) {
    j["documentation"] = obj.m_documentation.value();
  }
}

void LSPSpec::from_json(const json& j, ParameterInformation& obj) {
  j.at("label").get_to(obj.m_label);
  if (j.contains("documentation")) {
    obj.m_documentation = j.at("documentation").get<MarkupContent>();
  }
}

void LSPSpec::to_json(json& j, const SignatureInformation& obj) {
  j = json{{"label", obj.m_label}, {"parameters", obj.m_parameters}};
  if (obj.m_documentation) {
    j["documentation"] = obj.m_documentation.value();
  }
  if (obj.m_activeParameter) {
    j["activeParameter"] = obj.m_activeParameter.value();
  }
}

void LSPSpec::from_json(const json& j, SignatureInformation& obj) {
  j.at("label").get_to(obj.m_label);
  if (j.contains("documentation")) {
    obj.m_documentation = j.at("documentation").get<MarkupContent>();
  }
  if (j.contains("parameters")) {
    j.at("parameters").get_to(obj.m_parameters);
  }
  if (j.contains("activeParameter")) {
    obj.m_activeParameter = j.at("activeParameter").get<uint32_t>();
  }
}

void LSPSpec::to_json(json& j, const SignatureHelp& obj) {
  j = json{{"signatures", obj.m_signatures}, {"activeSignature", obj.m_activeSignature}};
  if (obj.m_activeParameter) {
    j["activeParameter"] = obj.m_activeParameter.value();
  }
}

void LSPSpec::from_json(const json& j, SignatureHelp& obj) {
  if (j.contains("signatures")) {
    j.at("signatures").get_to(obj.m_signatures);
  }
  if (j.contains("activeSignature")) {
    j.at("activeSignature").get_to(obj.m_activeSignature);
  }
  if (j.contains("activeParameter")) {
    obj.m_activeParameter = j.at("activeParameter").get<uint32_t>();
  }
}

void LSPSpec::to_json(json& j, const SignatureHelpParams& obj) {
  to_json(j, static_cast<const TextDocumentPositionParams&>(obj));
}

void LSPSpec::from_json(const json& j, SignatureHelpParams& obj) {
  from_json(j, static_cast<TextDocumentPositionParams&>(obj));
}
