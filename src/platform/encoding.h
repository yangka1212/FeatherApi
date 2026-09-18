#pragma once

#include <string>

std::wstring toWide(const std::string& value);
std::string toUtf8(const std::wstring& value);
