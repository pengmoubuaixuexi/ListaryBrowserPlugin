#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

std::wstring Trim(std::wstring_view value);
std::wstring ToLowerInvariant(std::wstring_view value);
std::string WideToUtf8(std::wstring_view value);
std::wstring Utf8ToWide(std::string_view value);
std::wstring ExpandEnvironment(std::wstring_view value);
std::vector<std::wstring> Split(std::wstring_view value, wchar_t delimiter);
bool StartsWithInsensitive(std::wstring_view value, std::wstring_view prefix);
std::wstring FormatWindowsError(unsigned long errorCode);
