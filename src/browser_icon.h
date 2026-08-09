#pragma once

#include <filesystem>
#include <string>
#include <string_view>

class BrowserIcon {
public:
    static bool ExportIco(const std::filesystem::path& sourceExecutable,
        const std::filesystem::path& destination, std::wstring& error);
    static std::filesystem::path CachedIcoPath(std::wstring_view browserId);
};
