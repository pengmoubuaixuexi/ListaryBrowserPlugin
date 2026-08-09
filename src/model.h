#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct BrowserDefinition {
    std::wstring id;
    std::wstring name;
    std::wstring prefix;
    std::wstring engine = L"chromium";
    bool enabled = true;
    std::filesystem::path iconPath;
    std::vector<std::filesystem::path> executableCandidates;
    std::vector<std::filesystem::path> userDataCandidates;
    std::filesystem::path historyRelativePath = L"History";
    std::wstring profileArgument = L"--profile-directory={profile}";
    std::vector<std::wstring> enabledProfiles;
};

struct BrowserProfile {
    std::wstring browserId;
    std::wstring directoryName;
    std::wstring displayName;
    std::filesystem::path userDataRoot;
    std::filesystem::path historyPath;
};

struct HistoryResult {
    std::wstring title;
    std::wstring url;
    std::wstring browserId;
    std::wstring browserName;
    std::wstring profileDirectory;
    std::int64_t lastVisitTime = 0;
    int typedCount = 0;
    int visitCount = 0;
    int relevance = 0;
    bool directUrl = false;
};

struct AppConfig {
    UINT hotkeyModifiers = MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
    UINT hotkeyVirtualKey = VK_SPACE;
    std::size_t maxResults = 20;
    UINT debounceMs = 90;
    std::vector<BrowserDefinition> browsers;
};

struct SearchResponse {
    std::uint64_t generation = 0;
    std::vector<HistoryResult> results;
    std::wstring error;
    std::uint64_t elapsedMicroseconds = 0;
    std::size_t profilesSearched = 0;
};
