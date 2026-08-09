#pragma once

#include "model.h"

#include <filesystem>
#include <string>
#include <string_view>

struct ListaryConfigurationResult {
    bool ok = false;
    std::filesystem::path preferencesPath;
    std::filesystem::path backupPath;
    std::wstring message;
};

class ListaryConfigurator {
public:
    static std::filesystem::path DetectPreferences();
    static bool StopForUpdate(const std::filesystem::path& executable,
        bool& wasRunning, std::wstring& error);
    static ListaryConfigurationResult Configure(const AppConfig& config,
        const std::filesystem::path& preferencesPath, const std::filesystem::path& statePath);
    static ListaryConfigurationResult ConfigureBrowser(const BrowserDefinition& browser,
        std::wstring_view previousPrefix, const std::filesystem::path& preferencesPath,
        const std::filesystem::path& statePath);
    static ListaryConfigurationResult Remove(const std::filesystem::path& preferencesPath,
        const std::filesystem::path& statePath);
};
