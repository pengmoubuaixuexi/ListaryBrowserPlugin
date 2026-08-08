#pragma once

#include "model.h"

#include <filesystem>
#include <string>
#include <string_view>

class BrowserLauncher {
public:
    static std::filesystem::path FindExecutable(const BrowserDefinition& browser);
    static bool OpenUrl(const BrowserDefinition& browser, std::wstring_view profileDirectory,
        std::wstring_view url, std::wstring& error);
    static std::wstring QuoteArgument(std::wstring_view argument);
};
