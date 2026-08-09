#include "browser_discovery.h"

#include "text_util.h"

#include <Windows.h>
#include <Shellapi.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
struct RegistryKeyCloser {
    void operator()(HKEY key) const { if (key) RegCloseKey(key); }
};
using RegistryKey = std::unique_ptr<std::remove_pointer_t<HKEY>, RegistryKeyCloser>;

struct DatabaseCloser {
    void operator()(sqlite3* database) const { if (database) sqlite3_close(database); }
};
using DatabasePtr = std::unique_ptr<sqlite3, DatabaseCloser>;

struct StatementCloser {
    void operator()(sqlite3_stmt* statement) const { if (statement) sqlite3_finalize(statement); }
};
using StatementPtr = std::unique_ptr<sqlite3_stmt, StatementCloser>;

struct RegisteredBrowser {
    std::wstring id;
    std::wstring name;
    std::filesystem::path executable;
    std::filesystem::path iconPath;
};

std::wstring ReadRegistryString(HKEY parent, const wchar_t* subkey, const wchar_t* valueName) {
    DWORD type = 0;
    DWORD bytes = 0;
    const LSTATUS sizeStatus = RegGetValueW(parent, subkey, valueName,
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, nullptr, &bytes);
    if (sizeStatus != ERROR_SUCCESS || bytes < sizeof(wchar_t) || bytes > 32768 * sizeof(wchar_t)) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(parent, subkey, valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            &type, value.data(), &bytes) != ERROR_SUCCESS) return {};
    std::size_t characters = bytes / sizeof(wchar_t);
    if (characters > 0 && value[characters - 1] == L'\0') --characters;
    value.resize(characters);
    return type == REG_EXPAND_SZ ? ExpandEnvironment(value) : value;
}

std::filesystem::path ExecutableFromCommand(std::wstring command) {
    command = Trim(command);
    if (command.empty()) return {};
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(command.c_str(), &count);
    std::filesystem::path executable;
    if (arguments && count > 0) executable = ExpandEnvironment(arguments[0]);
    if (arguments) LocalFree(arguments);
    std::error_code error;
    if (!executable.empty() && std::filesystem::is_regular_file(executable, error)) return executable;
    const auto lower = ToLowerInvariant(command);
    const auto extension = lower.find(L".exe");
    if (extension == std::wstring::npos) return {};
    std::wstring unquoted = Trim(std::wstring_view(command).substr(0, extension + 4));
    if (unquoted.size() >= 2 && unquoted.front() == L'"' && unquoted.back() == L'"') {
        unquoted = unquoted.substr(1, unquoted.size() - 2);
    }
    executable = ExpandEnvironment(unquoted);
    error.clear();
    return std::filesystem::is_regular_file(executable, error) ? executable : std::filesystem::path();
}

std::filesystem::path IconFromValue(std::wstring value, const std::filesystem::path& fallback) {
    value = Trim(value);
    if (value.empty()) return fallback;
    if (value.front() == L'"') {
        const auto closing = value.find(L'"', 1);
        if (closing != std::wstring::npos) value = value.substr(1, closing - 1);
    } else {
        const auto comma = value.rfind(L',');
        if (comma != std::wstring::npos) {
            const auto suffix = Trim(std::wstring_view(value).substr(comma + 1));
            const bool numeric = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](wchar_t ch) {
                return ch == L'-' || (ch >= L'0' && ch <= L'9');
            });
            if (numeric) value.resize(comma);
        }
    }
    const std::filesystem::path icon = ExpandEnvironment(Trim(value));
    std::error_code error;
    return std::filesystem::is_regular_file(icon, error) ? icon : fallback;
}

std::wstring NormalizedPath(const std::filesystem::path& path) {
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) normalized = path.lexically_normal();
    return ToLowerInvariant(normalized.wstring());
}

std::wstring Identifier(std::wstring_view value) {
    std::wstring result;
    for (const wchar_t ch : value) {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9')) result.push_back(ch);
        else if (ch >= L'A' && ch <= L'Z') result.push_back(static_cast<wchar_t>(std::towlower(ch)));
        else if (!result.empty() && result.back() != L'-') result.push_back(L'-');
    }
    while (!result.empty() && result.back() == L'-') result.pop_back();
    return result.empty() ? L"browser" : result;
}

std::wstring SearchToken(std::wstring_view value) {
    std::wstring result;
    for (const wchar_t ch : value) {
        if (std::iswalnum(ch)) result.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return result;
}

void EnumerateRegistryRoot(HKEY hive, REGSAM view, std::vector<RegisteredBrowser>& browsers) {
    HKEY rawRoot = nullptr;
    if (RegOpenKeyExW(hive, L"SOFTWARE\\Clients\\StartMenuInternet", 0,
            KEY_READ | view, &rawRoot) != ERROR_SUCCESS) return;
    RegistryKey root(rawRoot);
    for (DWORD index = 0;; ++index) {
        wchar_t name[512]{};
        DWORD length = static_cast<DWORD>(std::size(name));
        const LSTATUS status = RegEnumKeyExW(root.get(), index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;
        HKEY rawBrowser = nullptr;
        if (RegOpenKeyExW(root.get(), name, 0, KEY_READ | view, &rawBrowser) != ERROR_SUCCESS) continue;
        RegistryKey browser(rawBrowser);
        const auto command = ReadRegistryString(browser.get(), L"shell\\open\\command", nullptr);
        const auto executable = ExecutableFromCommand(command);
        if (executable.empty()) continue;
        std::wstring displayName = ReadRegistryString(browser.get(), nullptr, nullptr);
        if (displayName.empty()) displayName = ReadRegistryString(browser.get(), L"Capabilities", L"ApplicationName");
        if (displayName.empty()) displayName.assign(name, length);
        const auto iconValue = ReadRegistryString(browser.get(), L"DefaultIcon", nullptr);
        browsers.push_back(RegisteredBrowser{
            std::wstring(name, length), std::move(displayName), executable,
            IconFromValue(iconValue, executable)});
    }
}

std::vector<RegisteredBrowser> RegisteredBrowsers() {
    std::vector<RegisteredBrowser> browsers;
    EnumerateRegistryRoot(HKEY_CURRENT_USER, KEY_WOW64_64KEY, browsers);
    EnumerateRegistryRoot(HKEY_CURRENT_USER, KEY_WOW64_32KEY, browsers);
    EnumerateRegistryRoot(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, browsers);
    EnumerateRegistryRoot(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, browsers);
    std::set<std::wstring> seen;
    std::erase_if(browsers, [&](const RegisteredBrowser& browser) {
        return !seen.insert(NormalizedPath(browser.executable)).second;
    });
    return browsers;
}

enum class SchemaStatus { Compatible, Incompatible, ReadFailure };

SchemaStatus InspectSchema(const std::filesystem::path& history, std::wstring& detail) {
    sqlite3* raw = nullptr;
    const auto utf8 = WideToUtf8(history.wstring());
    const int status = sqlite3_open_v2(utf8.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    DatabasePtr database(raw);
    if (status != SQLITE_OK || !database) {
        detail = L"找到 History，但暂时无法只读验证数据库。";
        return SchemaStatus::ReadFailure;
    }
    sqlite3_busy_timeout(database.get(), 50);
    sqlite3_stmt* rawStatement = nullptr;
    if (sqlite3_prepare_v2(database.get(), "PRAGMA table_info(urls)", -1, &rawStatement, nullptr) != SQLITE_OK) {
        detail = L"History 数据库结构读取失败。";
        return SchemaStatus::ReadFailure;
    }
    StatementPtr statement(rawStatement);
    std::set<std::string> columns;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
        if (name) columns.insert(name);
    }
    if (step != SQLITE_DONE) {
        detail = L"History 数据库当前正忙。";
        return SchemaStatus::ReadFailure;
    }
    if (!columns.contains("url") || !columns.contains("title") || !columns.contains("last_visit_time")) {
        detail = L"History 不包含 Chromium urls 必需字段。";
        return SchemaStatus::Incompatible;
    }
    return SchemaStatus::Compatible;
}

bool HasRequiredSchema(const std::filesystem::path& history, std::wstring& detail) {
    const auto direct = InspectSchema(history, detail);
    if (direct == SchemaStatus::Compatible) return true;
    if (direct == SchemaStatus::Incompatible) return false;

    std::error_code error;
    const auto temporaryRoot = std::filesystem::temp_directory_path(error);
    if (error) {
        detail = L"无法定位临时目录以验证正在使用的 History。";
        return false;
    }
    const auto temporary = temporaryRoot /
        (L"BHL-Discovery-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L".db");
    std::filesystem::copy_file(history, temporary, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        detail = L"History 正在使用，且无法创建只读验证快照。";
        return false;
    }
    const auto snapshotStatus = InspectSchema(temporary, detail);
    std::filesystem::remove(temporary, error);
    return snapshotStatus == SchemaStatus::Compatible;
}

std::optional<std::pair<std::filesystem::path, std::wstring>> FindHistory(
    const std::filesystem::path& userDataRoot, std::wstring& detail) {
    std::error_code localStateError;
    if (!std::filesystem::is_regular_file(userDataRoot / L"Local State", localStateError)) {
        detail = L"未找到 Chromium Local State。";
        return std::nullopt;
    }
    std::error_code iteratorError;
    for (const auto& item : std::filesystem::directory_iterator(userDataRoot,
            std::filesystem::directory_options::skip_permission_denied, iteratorError)) {
        if (iteratorError) break;
        std::error_code typeError;
        if (!item.is_directory(typeError)) continue;
        const auto name = item.path().filename().wstring();
        if (name != L"Default" && !StartsWithInsensitive(name, L"Profile ")) continue;
        const auto history = item.path() / L"History";
        std::error_code historyError;
        if (std::filesystem::is_regular_file(history, historyError)) {
            return std::make_pair(history, name);
        }
    }
    detail = L"未找到可读取的 Default/Profile History。";
    return std::nullopt;
}

std::vector<std::filesystem::path> ShallowUserDataRoots() {
    std::vector<std::filesystem::path> roots;
    std::set<std::wstring> seen;
    auto consider = [&](const std::filesystem::path& root) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(root / L"Local State", error)) return;
        const auto normalized = NormalizedPath(root);
        if (seen.insert(normalized).second) roots.push_back(root);
    };
    for (const auto& baseText : {ExpandEnvironment(L"%LocalAppData%"), ExpandEnvironment(L"%AppData%")}) {
        const std::filesystem::path base(baseText);
        std::error_code error;
        for (const auto& first : std::filesystem::directory_iterator(base,
                std::filesystem::directory_options::skip_permission_denied, error)) {
            if (error) break;
            std::error_code firstTypeError;
            if (!first.is_directory(firstTypeError)) continue;
            consider(first.path());
            consider(first.path() / L"User Data");
            std::error_code secondError;
            for (const auto& second : std::filesystem::directory_iterator(first.path(),
                    std::filesystem::directory_options::skip_permission_denied, secondError)) {
                if (secondError) break;
                std::error_code secondTypeError;
                if (!second.is_directory(secondTypeError)) continue;
                consider(second.path());
                consider(second.path() / L"User Data");
            }
        }
    }
    return roots;
}

int RootMatchScore(const RegisteredBrowser& registered, const std::filesystem::path& root) {
    const std::wstring browserToken = SearchToken(registered.id + L" " + registered.name + L" " +
        registered.executable.stem().wstring());
    const auto owner = root.filename() == L"User Data" ? root.parent_path() : root;
    const std::wstring leaf = SearchToken(owner.filename().wstring());
    const std::wstring parent = SearchToken(owner.parent_path().filename().wstring());
    int score = 0;
    if (leaf.size() >= 4 && browserToken.find(leaf) != std::wstring::npos) score += 10;
    if (parent.size() >= 4 && browserToken.find(parent) != std::wstring::npos) score += 4;
    const std::wstring executable = SearchToken(registered.executable.stem().wstring());
    if (executable.size() >= 4 && leaf.find(executable) != std::wstring::npos) score += 8;
    return score;
}

const BrowserDefinition* FindConfiguredBrowser(const AppConfig& config,
    const std::filesystem::path& executable) {
    const auto normalized = NormalizedPath(executable);
    for (const auto& browser : config.browsers) {
        for (const auto& candidate : browser.executableCandidates) {
            if (NormalizedPath(candidate) == normalized) return &browser;
        }
    }
    return nullptr;
}

std::wstring UniquePrefix(const AppConfig& config, std::wstring_view name) {
    const std::wstring token = SearchToken(name);
    std::wstring prefix = token.empty() ? L"b" : token.substr(0, 1);
    const auto used = [&](std::wstring_view value) {
        return std::any_of(config.browsers.begin(), config.browsers.end(), [&](const BrowserDefinition& browser) {
            return ToLowerInvariant(browser.prefix) == ToLowerInvariant(value);
        });
    };
    if (!used(prefix)) return prefix;
    for (std::size_t length = 2; length <= std::min<std::size_t>(token.size(), 4); ++length) {
        prefix = token.substr(0, length);
        if (!used(prefix)) return prefix;
    }
    for (int suffix = 2; suffix < 100; ++suffix) {
        prefix = (token.empty() ? L"b" : token.substr(0, 1)) + std::to_wstring(suffix);
        if (!used(prefix)) return prefix;
    }
    return L"browser";
}
}

bool BrowserDiscovery::IsCompatibleUserDataRoot(const std::filesystem::path& userDataRoot,
    std::wstring& detail) {
    const auto history = FindHistory(userDataRoot, detail);
    if (!history || !HasRequiredSchema(history->first, detail)) return false;
    detail = L"已验证 Chromium History（" + history->second + L"）。";
    return true;
}

BrowserDiscoveryReport BrowserDiscovery::Scan(const AppConfig& config) {
    BrowserDiscoveryReport report;
    const auto roots = ShallowUserDataRoots();
    for (const auto& registered : RegisteredBrowsers()) {
        BrowserDiscoveryEntry entry;
        entry.registrationId = registered.id;
        entry.name = registered.name;
        entry.executable = registered.executable;
        entry.iconPath = registered.iconPath;

        const BrowserDefinition* configured = FindConfiguredBrowser(config, registered.executable);
        if (configured) {
            entry.definition = *configured;
            entry.definition.executableCandidates.insert(entry.definition.executableCandidates.begin(), registered.executable);
            if (entry.definition.iconPath.empty()) entry.definition.iconPath = registered.iconPath;
            for (const auto& candidate : entry.definition.userDataCandidates) {
                const auto history = FindHistory(candidate, entry.detail);
                if (history) {
                    entry.userDataRoot = candidate;
                    entry.chromiumCompatible = true;
                    entry.detail = L"已确认配置的 Chromium History 布局（" + history->second + L"）。";
                    break;
                }
            }
        } else {
            int bestScore = 0;
            std::filesystem::path bestRoot;
            std::wstring bestDetail;
            for (const auto& root : roots) {
                const int score = RootMatchScore(registered, root);
                if (score <= bestScore) continue;
                std::wstring detail;
                if (!IsCompatibleUserDataRoot(root, detail)) continue;
                bestScore = score;
                bestRoot = root;
                bestDetail = std::move(detail);
            }
            if (!bestRoot.empty()) {
                entry.chromiumCompatible = true;
                entry.userDataRoot = bestRoot;
                entry.detail = std::move(bestDetail);
                entry.definition.id = Identifier(registered.id);
                entry.definition.name = registered.name;
                entry.definition.prefix = UniquePrefix(config, registered.name);
                entry.definition.engine = L"chromium";
                entry.definition.enabled = false;
                entry.definition.iconPath = registered.iconPath;
                entry.definition.executableCandidates = {registered.executable};
                entry.definition.userDataCandidates = {bestRoot};
                entry.definition.enabledProfiles = {L"*"};
            }
        }
        if (!entry.chromiumCompatible && entry.detail.empty()) {
            entry.detail = L"已在 Windows 注册，但未发现兼容的 Chromium History。";
        }
        report.entries.push_back(std::move(entry));
    }
    return report;
}

void BrowserDiscovery::MergeCompatible(AppConfig& config, const BrowserDiscoveryReport& report) {
    for (const auto& entry : report.entries) {
        if (!entry.chromiumCompatible) continue;
        auto existing = std::find_if(config.browsers.begin(), config.browsers.end(), [&](const BrowserDefinition& browser) {
            if (browser.id == entry.definition.id) return true;
            const auto discoveredExecutable = NormalizedPath(entry.executable);
            return std::any_of(browser.executableCandidates.begin(), browser.executableCandidates.end(),
                [&](const std::filesystem::path& candidate) { return NormalizedPath(candidate) == discoveredExecutable; });
        });
        if (existing == config.browsers.end()) {
            config.browsers.push_back(entry.definition);
            continue;
        }
        if (std::none_of(existing->executableCandidates.begin(), existing->executableCandidates.end(),
                [&](const std::filesystem::path& candidate) {
                    return NormalizedPath(candidate) == NormalizedPath(entry.executable);
                })) {
            existing->executableCandidates.insert(existing->executableCandidates.begin(), entry.executable);
        }
        if (!entry.userDataRoot.empty() &&
            std::none_of(existing->userDataCandidates.begin(), existing->userDataCandidates.end(),
                [&](const std::filesystem::path& candidate) {
                    return NormalizedPath(candidate) == NormalizedPath(entry.userDataRoot);
                })) {
            existing->userDataCandidates.insert(existing->userDataCandidates.begin(), entry.userDataRoot);
        }
        if (existing->iconPath.empty()) existing->iconPath = entry.iconPath;
    }
}

std::wstring BrowserDiscoveryReport::Summary() const {
    const auto supported = static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(),
        [](const BrowserDiscoveryEntry& entry) { return entry.chromiumCompatible; }));
    return L"Windows 检测到 " + std::to_wstring(entries.size()) + L" 个已注册浏览器，其中 " +
        std::to_wstring(supported) + L" 个可读取 Chromium 历史。";
}
