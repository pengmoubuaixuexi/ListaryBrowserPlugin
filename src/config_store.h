#pragma once

#include "model.h"

#include <filesystem>
#include <string>
#include <vector>

class ConfigStore {
public:
    static AppConfig Load(const std::filesystem::path& iniPath, std::wstring& warning);
    static bool Validate(const AppConfig& config, std::wstring& error);
    static bool SaveBrowserSettings(const std::filesystem::path& iniPath,
        const BrowserDefinition& browser, std::wstring& error);
    static bool SaveBrowserSettings(const std::filesystem::path& iniPath,
        const std::vector<BrowserDefinition>& browsers, std::wstring& error);
    static bool SaveListarySettings(const std::filesystem::path& iniPath,
        const std::vector<BrowserDefinition>& browsers, const BluetoothConfig& bluetooth,
        std::wstring& error);
    static AppConfig Defaults();
};
