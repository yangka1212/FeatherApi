#pragma once

#include <string>

std::string prettyJson(const std::string& value, bool compact = false);
bool isValidJson(const std::string& value);
std::wstring jsonValidationMessage(const std::string& value, bool includeColumn = true);
