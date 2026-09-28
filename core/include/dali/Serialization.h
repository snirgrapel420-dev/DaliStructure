#pragma once
#include "dali/Json.h"
#include "dali/Model.h"
#include <string>

namespace dali {

json::Value toJson(const StructureDocument& doc);
// Returns false (and leaves doc untouched) on malformed or newer-schema data.
bool fromJson(const json::Value& v, StructureDocument& doc, std::string* error = nullptr);

std::string serialize(const StructureDocument& doc, bool pretty = false);
bool deserialize(const std::string& text, StructureDocument& doc, std::string* error = nullptr);

} // namespace dali
