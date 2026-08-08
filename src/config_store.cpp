#include "config_store.h"

#include "text_util.h"

#include <Windows.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <set>

namespace {
std::wstring ReadValue(const std::filesystem::path& path, const std::wstring& section,
    const wchar_t* key, const wchar_t* fallback) {
    std::wstring buffer(32768, L'\0');
    const DWORD count = GetPrivateProfileStringW(section.c_str(), key, fallback, buffer.data(),
        static_cast<DWORD>(buffer.size()), path.c_str());
    buffer.resize(count);
    return buffer;
}

bool ReadBool(const std::filesystem::path& path, const std::wstring& section,
    const wchar_t* key, bool fallback) {
    const auto value = ToLowerInvariant(ReadValue(path, section, key, fallback ? L"true" : L"false"));
    return value == L"true" || value == L"1" || value == L"yes";
}

std::vector<std::filesystem::path> ReadPaths(const std::filesystem::path& path,
    const std::wstring& section, const wchar_t* key) {
    std::vector<std::filesystem::path> result;
    for (const auto& entry : Split(ReadValue(path, section, key, L""), L'|')) {
        result.emplace_back(ExpandEnvironment(entry));
    }
    return result;
}

void ParseHotkey(std::wstring_view text, AppConfig& config, std::wstring& warning) {
    const auto normalizedText = ToLowerInvariant(Trim(text));
    if (normalizedText == L"none" || normalizedText == L"off" || normalizedText == L"disabled") {
        config.hotkeyModifiers = 0;
        config.hotkeyVirtualKey = 0;
        return;
    }
    UINT modifiers = MOD_NOREPEAT;
    UINT key = 0;
    for (auto part : Split(text, L'+')) {
        part = ToLowerInvariant(part);
        if (part == L"alt") modifiers |= MOD_ALT;
        else if (part == L"shift") modifiers |= MOD_SHIFT;
        else if (part == L"ctrl" || part == L"control") modifiers |= MOD_CONTROL;
        else if (part == L"win" || part == L"windows") modifiers |= MOD_WIN;
        else if (part == L"space") key = VK_SPACE;
        else if (part.size() == 1 && ((part[0] >= L'a' && part[0] <= L'z') ||
                 (part[0] >= L'0' && part[0] <= L'9'))) {
            key = static_cast<UINT>(std::towupper(part[0]));
        }
    }
    if (key == 0 || (modifiers & ~MOD_NOREPEAT) == 0) {
        warning = L"快捷键配置无效，已使用 Alt+Shift+Space。";
        return;
    }
    config.hotkeyModifiers = modifiers;
    config.hotkeyVirtualKey = key;
}

std::vector<std::wstring> EnumerateBrowserSections(const std::filesystem::path& path) {
    std::vector<wchar_t> buffer(65536, L'\0');
    const DWORD count = GetPrivateProfileSectionNamesW(buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    std::vector<std::wstring> result;
    for (const wchar_t* current = buffer.data(); count > 0 && *current; current += std::wcslen(current) + 1) {
        std::wstring section(current);
        if (StartsWithInsensitive(section, L"browser.")) {
            result.push_back(std::move(section));
        }
    }
    return result;
}
}

AppConfig ConfigStore::Defaults() {
    AppConfig config;
    BrowserDefinition chrome;
    chrome.id = L"chrome";
    chrome.name = L"Google Chrome";
    chrome.prefix = L"g";
    chrome.executableCandidates = {
        ExpandEnvironment(L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe"),
        ExpandEnvironment(L"%LocalAppData%\\Google\\Chrome\\Application\\chrome.exe")};
    chrome.userDataCandidates = {ExpandEnvironment(L"%LocalAppData%\\Google\\Chrome\\User Data")};

    BrowserDefinition edge;
    edge.id = L"edge";
    edge.name = L"Microsoft Edge";
    edge.prefix = L"e";
    edge.executableCandidates = {
        ExpandEnvironment(L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe"),
        ExpandEnvironment(L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe")};
    edge.userDataCandidates = {ExpandEnvironment(L"%LocalAppData%\\Microsoft\\Edge\\User Data")};
    config.browsers = {std::move(chrome), std::move(edge)};
    return config;
}

AppConfig ConfigStore::Load(const std::filesystem::path& iniPath, std::wstring& warning) {
    AppConfig config = Defaults();
    if (!std::filesystem::exists(iniPath)) {
        warning = L"未找到配置文件，已使用内置安全默认值。";
        return config;
    }

    config.maxResults = std::clamp<std::size_t>(std::wcstoul(
        ReadValue(iniPath, L"app", L"MaxResults", L"20").c_str(), nullptr, 10), 1, 100);
    config.debounceMs = std::clamp<UINT>(std::wcstoul(
        ReadValue(iniPath, L"app", L"DebounceMs", L"90").c_str(), nullptr, 10), 60, 120);
    ParseHotkey(ReadValue(iniPath, L"app", L"Hotkey", L"Alt+Shift+Space"), config, warning);

    const auto sections = EnumerateBrowserSections(iniPath);
    if (sections.empty()) {
        warning = L"配置文件未包含 browser.* 节，已使用内置浏览器定义。";
        return config;
    }

    config.browsers.clear();
    for (const auto& section : sections) {
        BrowserDefinition browser;
        browser.id = section.substr(std::wstring(L"browser.").size());
        browser.name = ReadValue(iniPath, section, L"Name", browser.id.c_str());
        browser.prefix = ToLowerInvariant(ReadValue(iniPath, section, L"Prefix", L""));
        browser.engine = ToLowerInvariant(ReadValue(iniPath, section, L"Engine", L"chromium"));
        browser.enabled = ReadBool(iniPath, section, L"Enabled", true);
        browser.executableCandidates = ReadPaths(iniPath, section, L"ExecutableCandidates");
        browser.userDataCandidates = ReadPaths(iniPath, section, L"UserDataCandidates");
        browser.historyRelativePath = ReadValue(iniPath, section, L"HistoryRelativePath", L"History");
        browser.profileArgument = ReadValue(iniPath, section, L"ProfileArgument", L"--profile-directory={profile}");
        browser.enabledProfiles = Split(ReadValue(iniPath, section, L"EnabledProfiles", L"*"), L'|');
        config.browsers.push_back(std::move(browser));
    }
    return config;
}

bool ConfigStore::Validate(const AppConfig& config, std::wstring& error) {
    std::set<std::wstring> prefixes;
    for (const auto& browser : config.browsers) {
        if (!browser.enabled) continue;
        if (browser.id.empty() || browser.name.empty() || browser.prefix.empty()) {
            error = L"浏览器配置缺少 ID、名称或前缀。";
            return false;
        }
        if (browser.prefix.find_first_of(L" \t\r\n") != std::wstring::npos) {
            error = L"浏览器前缀不能包含空白字符：" + browser.prefix;
            return false;
        }
        if (browser.engine != L"chromium") {
            error = L"首版仅支持 chromium 引擎：" + browser.id;
            return false;
        }
        const auto normalized = ToLowerInvariant(browser.prefix);
        if (!prefixes.insert(normalized).second) {
            error = L"浏览器前缀冲突：" + browser.prefix;
            return false;
        }
        if (browser.executableCandidates.empty() || browser.userDataCandidates.empty()) {
            error = L"浏览器配置缺少 EXE 或 User Data 候选路径：" + browser.id;
            return false;
        }
    }
    return true;
}
