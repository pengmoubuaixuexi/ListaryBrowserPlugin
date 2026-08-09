#pragma once

#include "model.h"

#include <filesystem>
#include <string>
#include <vector>

struct BrowserDiscoveryEntry {
    std::wstring registrationId;
    std::wstring name;
    std::filesystem::path executable;
    std::filesystem::path iconPath;
    std::filesystem::path userDataRoot;
    bool chromiumCompatible = false;
    std::wstring detail;
    BrowserDefinition definition;
};

struct BrowserDiscoveryReport {
    std::vector<BrowserDiscoveryEntry> entries;
    std::wstring Summary() const;
};

class BrowserDiscovery {
public:
    static BrowserDiscoveryReport Scan(const AppConfig& config);
    static void MergeCompatible(AppConfig& config, const BrowserDiscoveryReport& report);
    static bool IsCompatibleUserDataRoot(const std::filesystem::path& userDataRoot,
        std::wstring& detail);
};
