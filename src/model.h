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

enum class BluetoothTransport {
    Classic,
    LowEnergy,
    DualMode
};

struct BluetoothDeviceTarget {
    std::wstring stableKey;
    std::wstring displayName;
    std::wstring classicDeviceId;
    std::wstring lowEnergyDeviceId;
    BluetoothTransport transport = BluetoothTransport::Classic;
    bool connected = false;
    bool present = false;
};

struct BluetoothConfig {
    bool enabled = true;
    std::wstring keyword = L"ly";
    UINT cacheSeconds = 8;
    UINT connectTimeoutMs = 20000;
};

struct BluetoothEnumerationResult {
    std::vector<BluetoothDeviceTarget> devices;
    std::wstring error;
    std::uint64_t elapsedMilliseconds = 0;
    std::uint64_t workerPrivateWorkingSetBytes = 0;
    std::uint64_t workerPrivateBytes = 0;
    unsigned workerThreads = 0;
    unsigned workerHandles = 0;
};

struct BluetoothConnectionResult {
    bool attempted = false;
    bool requestAccepted = false;
    bool confirmedConnected = false;
    std::wstring message;
    std::uint64_t elapsedMilliseconds = 0;
    std::uint64_t workerPrivateWorkingSetBytes = 0;
    std::uint64_t workerPrivateBytes = 0;
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
    BluetoothConfig bluetooth;
    std::vector<BrowserDefinition> browsers;
};

enum class ListaryActionKind {
    BrowserOpen,
    BluetoothConnect
};

struct ListaryAction {
    ListaryActionKind kind = ListaryActionKind::BrowserOpen;
    HistoryResult browserResult;
    BluetoothDeviceTarget bluetoothTarget;
};

struct SearchResponse {
    std::uint64_t generation = 0;
    std::vector<HistoryResult> results;
    std::wstring error;
    std::uint64_t elapsedMicroseconds = 0;
    std::size_t profilesSearched = 0;
};
