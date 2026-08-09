#include "listary_configurator.h"

#include "browser_launcher.h"
#include "json_document.h"
#include "text_util.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {
constexpr wchar_t kGoogleWebSearchId[] = L"ecb51462-cb27-4b89-ae53-333b4550f489";

bool ReadBytes(const std::filesystem::path& path, std::string& bytes, std::wstring& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = L"无法读取 Listary 配置：" + path.wstring();
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        error = L"读取 Listary 配置失败：" + path.wstring();
        return false;
    }
    return true;
}

std::filesystem::path BackupPath(const std::filesystem::path& path) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::wostringstream name;
    name << path.filename().wstring() << L".bhl-backup-" << std::setfill(L'0')
         << std::setw(4) << time.wYear << std::setw(2) << time.wMonth << std::setw(2) << time.wDay
         << L'-' << std::setw(2) << time.wHour << std::setw(2) << time.wMinute << std::setw(2) << time.wSecond
         << L'-' << std::setw(3) << time.wMilliseconds;
    return path.parent_path() / name.str();
}

bool WriteAtomic(const std::filesystem::path& path, std::string_view bytes,
    std::filesystem::path& backup, std::wstring& error) {
    backup = BackupPath(path);
    std::error_code copyError;
    std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, copyError);
    if (copyError) {
        error = L"无法备份 Listary 配置：" + Utf8ToWide(copyError.message());
        return false;
    }
    const auto temporary = path.parent_path() /
        (path.filename().wstring() + L".bhl-temp-" + std::to_wstring(GetCurrentProcessId()));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary, copyError);
            error = L"无法写入 Listary 临时配置。";
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::filesystem::remove(temporary, copyError);
        error = L"无法替换 Listary 配置：" + FormatWindowsError(code);
        return false;
    }
    return true;
}

JsonValue* RequireObject(JsonValue& parent, std::wstring_view name, std::wstring& error) {
    JsonValue* value = parent.Find(name);
    if (!value || value->type() != JsonValue::Type::Object) {
        error = L"Listary 配置结构不兼容：缺少对象 " + std::wstring(name) + L"。";
        return nullptr;
    }
    return value;
}

JsonValue* RequireArray(JsonValue& parent, std::wstring_view name, std::wstring& error) {
    JsonValue* value = parent.Find(name);
    if (!value || value->type() != JsonValue::Type::Array) {
        error = L"Listary 配置结构不兼容：缺少数组 " + std::wstring(name) + L"。";
        return nullptr;
    }
    return value;
}

const JsonValue* ItemObject(const JsonValue& insertion) {
    const JsonValue* item = insertion.Find(L"Item");
    return item && item->type() == JsonValue::Type::Object ? item : nullptr;
}

std::wstring StringProperty(const JsonValue& object, std::wstring_view name) {
    const JsonValue* value = object.Find(name);
    return value && value->type() == JsonValue::Type::String ? value->text() : L"";
}

bool IsOurInsertion(const JsonValue& insertion) {
    const JsonValue* item = ItemObject(insertion);
    if (!item) return false;
    return StartsWithInsensitive(StringProperty(*item, L"Url"), L"bhl://open?") ||
        StartsWithInsensitive(StringProperty(*item, L"SuggestionUrl"), L"http://127.0.0.1:32119/suggest?");
}

std::wstring UrlEncode(std::wstring_view value) {
    const std::string utf8 = WideToUtf8(value);
    std::wostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (unsigned char ch : utf8) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded << static_cast<wchar_t>(ch);
        } else {
            encoded << L'%' << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(ch);
        }
    }
    return encoded.str();
}

JsonValue MakeInsertion(const BrowserDefinition& browser) {
    const std::wstring encodedPrefix = UrlEncode(browser.prefix);
    std::wstring title = browser.name;
    if (browser.id == L"chrome") title = L"Chrome 历史";
    else if (browser.id == L"edge") title = L"Edge 历史";

    JsonValue icon = JsonValue::ObjectValue();
    const auto executable = BrowserLauncher::FindExecutable(browser);
    std::error_code iconError;
    const auto iconPath = !browser.iconPath.empty() && std::filesystem::is_regular_file(browser.iconPath, iconError)
        ? browser.iconPath : executable;
    icon.Set(L"Path", JsonValue::String(iconPath.wstring()));
    icon.Set(L"TypeName", JsonValue::String(L"Path"));

    JsonValue item = JsonValue::ObjectValue();
    item.Set(L"Keyword", JsonValue::String(browser.prefix));
    item.Set(L"Url", JsonValue::String(L"bhl://open?prefix=" + encodedPrefix + L"&selection={query}"));
    item.Set(L"Title", JsonValue::String(std::move(title)));
    item.Set(L"Icon", std::move(icon));
    item.Set(L"SuggestionProvider", JsonValue::String(L"Custom"));
    item.Set(L"SuggestionUrl", JsonValue::String(
        L"http://127.0.0.1:32119/suggest?prefix=" + encodedPrefix + L"&q={query}"));

    JsonValue insertion = JsonValue::ObjectValue();
    insertion.Set(L"Index", JsonValue::Number(L"-1"));
    insertion.Set(L"Info", JsonValue());
    insertion.Set(L"Item", std::move(item));
    return insertion;
}

bool HasDisabledGoogleUpdate(const JsonValue::Array& updates) {
    for (const auto& update : updates) {
        if (StringProperty(update, L"Id") != kGoogleWebSearchId) continue;
        const JsonValue* properties = update.Find(L"UpdatedProperties");
        const JsonValue* enabled = properties ? properties->Find(L"Enabled") : nullptr;
        if (enabled && enabled->type() == JsonValue::Type::Boolean && !enabled->boolean()) return true;
    }
    return false;
}

void AddDisabledGoogleUpdate(JsonValue::Array& updates) {
    JsonValue properties = JsonValue::ObjectValue();
    properties.Set(L"Enabled", JsonValue::Boolean(false));
    JsonValue update = JsonValue::ObjectValue();
    update.Set(L"Id", JsonValue::String(kGoogleWebSearchId));
    update.Set(L"UpdatedProperties", std::move(properties));
    updates.push_back(std::move(update));
}

void RemoveGoogleUpdate(JsonValue::Array& updates) {
    std::erase_if(updates, [](const JsonValue& update) {
        return StringProperty(update, L"Id") == kGoogleWebSearchId;
    });
}

bool ReadState(const std::filesystem::path& statePath) {
    return GetPrivateProfileIntW(L"listary", L"GoogleDisabledByPlugin", 0, statePath.c_str()) != 0;
}

bool ReadPerBrowserMode(const std::filesystem::path& statePath) {
    return GetPrivateProfileIntW(L"listary", L"PerBrowserMode", 0, statePath.c_str()) != 0;
}

void WriteState(const std::filesystem::path& statePath, bool googleDisabledByPlugin) {
    WritePrivateProfileStringW(L"listary", L"GoogleDisabledByPlugin",
        googleDisabledByPlugin ? L"1" : L"0", statePath.c_str());
}

void WritePerBrowserMode(const std::filesystem::path& statePath) {
    WritePrivateProfileStringW(L"listary", L"PerBrowserMode", L"1", statePath.c_str());
}

bool IsOurInsertionForPrefix(const JsonValue& insertion, std::wstring_view prefix) {
    if (!IsOurInsertion(insertion)) return false;
    const JsonValue* item = ItemObject(insertion);
    return item && ToLowerInvariant(StringProperty(*item, L"Keyword")) == ToLowerInvariant(prefix);
}

bool HasOurGoogleInsertion(const JsonValue::Array& insertions) {
    return std::any_of(insertions.begin(), insertions.end(), [](const JsonValue& insertion) {
        return IsOurInsertionForPrefix(insertion, L"g");
    });
}

bool LoadDocument(const std::filesystem::path& path, JsonValue& root, std::wstring& error) {
    std::string bytes;
    if (!ReadBytes(path, bytes, error) || !ParseJsonUtf8(bytes, root, error)) return false;
    if (root.type() != JsonValue::Type::Object) {
        error = L"Listary 配置根节点不是 JSON 对象。";
        return false;
    }
    return true;
}

bool LocateWebSearch(JsonValue& root, JsonValue*& insertions, JsonValue*& updates, std::wstring& error) {
    JsonValue* webSearch = RequireObject(root, L"WebSearch", error);
    JsonValue* items = webSearch ? RequireObject(*webSearch, L"Items", error) : nullptr;
    insertions = items ? RequireArray(*items, L"Insertions", error) : nullptr;
    updates = items ? RequireArray(*items, L"Updates", error) : nullptr;
    return insertions && updates;
}

std::wstring NormalizedExecutable(const std::filesystem::path& path) {
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) normalized = path.lexically_normal();
    return ToLowerInvariant(normalized.wstring());
}

std::vector<DWORD> MatchingProcesses(const std::filesystem::path& executable) {
    std::vector<DWORD> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return processes;
    PROCESSENTRY32W entry{sizeof(entry)};
    const auto expected = NormalizedExecutable(executable);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, executable.filename().c_str()) != 0) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            std::wstring path(32768, L'\0');
            DWORD length = static_cast<DWORD>(path.size());
            const bool matched = QueryFullProcessImageNameW(process, 0, path.data(), &length) &&
                NormalizedExecutable(std::filesystem::path(std::wstring(path.data(), length))) == expected;
            CloseHandle(process);
            if (matched) processes.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return processes;
}

struct WindowProcessContext {
    const std::vector<DWORD>* processes = nullptr;
    bool visible = false;
    bool requestClose = false;
};

BOOL CALLBACK VisitListaryWindow(HWND window, LPARAM parameter) {
    auto& context = *reinterpret_cast<WindowProcessContext*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (std::find(context.processes->begin(), context.processes->end(), processId) ==
        context.processes->end()) return TRUE;
    if (IsWindowVisible(window)) context.visible = true;
    if (context.requestClose) PostMessageW(window, WM_CLOSE, 0, 0);
    return TRUE;
}

bool WaitForProcessExit(const std::filesystem::path& executable, DWORD milliseconds) {
    const auto deadline = GetTickCount64() + milliseconds;
    while (GetTickCount64() < deadline) {
        if (MatchingProcesses(executable).empty()) return true;
        Sleep(50);
    }
    return MatchingProcesses(executable).empty();
}
}

std::filesystem::path ListaryConfigurator::DetectPreferences() {
    const auto appData = ExpandEnvironment(L"%APPDATA%");
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::path(appData) / L"Listary" / L"UserProfile" / L"Settings" / L"Preferences.json",
        std::filesystem::path(appData) / L"Listary" / L"Preferences.json"};
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    return {};
}

bool ListaryConfigurator::StopForUpdate(const std::filesystem::path& executable,
    bool& wasRunning, std::wstring& error) {
    wasRunning = false;
    if (executable.empty()) return true;
    auto processes = MatchingProcesses(executable);
    if (processes.empty()) return true;
    wasRunning = true;

    WindowProcessContext inspect{&processes};
    EnumWindows(VisitListaryWindow, reinterpret_cast<LPARAM>(&inspect));
    if (inspect.visible) {
        error = L"Listary 当前有可见窗口，可能正在使用。请完成当前操作后点击“重试”，或选择“取消”。";
        return false;
    }

    WindowProcessContext close{&processes, false, true};
    EnumWindows(VisitListaryWindow, reinterpret_cast<LPARAM>(&close));
    if (WaitForProcessExit(executable, 1000)) return true;

    processes = MatchingProcesses(executable);
    for (const DWORD processId : processes) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId);
        if (!process) {
            error = L"无法关闭 Listary：" + FormatWindowsError(GetLastError());
            return false;
        }
        const bool terminated = TerminateProcess(process, 0) != FALSE;
        const DWORD terminateError = terminated ? ERROR_SUCCESS : GetLastError();
        if (terminated) WaitForSingleObject(process, 3000);
        CloseHandle(process);
        if (!terminated) {
            error = L"无法关闭 Listary：" + FormatWindowsError(terminateError);
            return false;
        }
    }
    if (!WaitForProcessExit(executable, 1000)) {
        error = L"Listary 尚未完全退出，请稍后重试。";
        return false;
    }
    return true;
}

ListaryConfigurationResult ListaryConfigurator::Configure(const AppConfig& config,
    const std::filesystem::path& preferencesPath, const std::filesystem::path& statePath) {
    ListaryConfigurationResult result;
    result.preferencesPath = preferencesPath;
    if (preferencesPath.empty() || !std::filesystem::is_regular_file(preferencesPath)) {
        result.message = L"未找到受支持的 Listary 配置文件。";
        return result;
    }

    JsonValue root;
    if (!LoadDocument(preferencesPath, root, result.message)) return result;
    JsonValue* insertions = nullptr;
    JsonValue* updates = nullptr;
    if (!LocateWebSearch(root, insertions, updates, result.message)) return result;

    auto& insertionArray = insertions->array();
    std::erase_if(insertionArray, IsOurInsertion);

    for (const auto& browser : config.browsers) {
        if (!browser.enabled) continue;
        for (const auto& existing : insertionArray) {
            const JsonValue* item = ItemObject(existing);
            if (item && ToLowerInvariant(StringProperty(*item, L"Keyword")) == ToLowerInvariant(browser.prefix)) {
                result.message = L"Listary 关键字已被其他自定义项目占用：" + browser.prefix;
                return result;
            }
        }
    }

    bool needsGoogleDisabled = false;
    for (const auto& browser : config.browsers) {
        if (!browser.enabled) continue;
        insertionArray.push_back(MakeInsertion(browser));
        if (ToLowerInvariant(browser.prefix) == L"g") needsGoogleDisabled = true;
    }

    auto& updateArray = updates->array();
    const bool googleAlreadyDisabled = HasDisabledGoogleUpdate(updateArray);
    bool googleDisabledByPlugin = ReadState(statePath);
    if (needsGoogleDisabled && !googleAlreadyDisabled) {
        AddDisabledGoogleUpdate(updateArray);
        googleDisabledByPlugin = true;
    } else if (!needsGoogleDisabled && googleDisabledByPlugin) {
        RemoveGoogleUpdate(updateArray);
        googleDisabledByPlugin = false;
    }

    const std::string serialized = SerializeJsonUtf8(root);
    JsonValue verification;
    std::wstring verificationError;
    if (!ParseJsonUtf8(serialized, verification, verificationError)) {
        result.message = L"生成的 Listary 配置校验失败：" + verificationError;
        return result;
    }
    if (!WriteAtomic(preferencesPath, serialized, result.backupPath, result.message)) return result;
    WriteState(statePath, googleDisabledByPlugin);
    result.ok = true;
    result.message = L"Listary 快速配置已完成。备份：" + result.backupPath.wstring();
    return result;
}

ListaryConfigurationResult ListaryConfigurator::ConfigureBrowser(const BrowserDefinition& browser,
    std::wstring_view previousPrefix, const std::filesystem::path& preferencesPath,
    const std::filesystem::path& statePath) {
    ListaryConfigurationResult result;
    result.preferencesPath = preferencesPath;
    if (preferencesPath.empty() || !std::filesystem::is_regular_file(preferencesPath)) {
        result.message = L"未找到受支持的 Listary 配置文件。";
        return result;
    }

    JsonValue root;
    if (!LoadDocument(preferencesPath, root, result.message)) return result;
    JsonValue* insertions = nullptr;
    JsonValue* updates = nullptr;
    if (!LocateWebSearch(root, insertions, updates, result.message)) return result;

    auto& insertionArray = insertions->array();
    if (!ReadPerBrowserMode(statePath)) {
        // Migrate configurations produced by older versions, which wrote every
        // Enabled browser even though the dialog only showed one selection.
        std::erase_if(insertionArray, IsOurInsertion);
    } else {
        std::erase_if(insertionArray, [&](const JsonValue& insertion) {
            return IsOurInsertionForPrefix(insertion, previousPrefix) ||
                IsOurInsertionForPrefix(insertion, browser.prefix);
        });
    }

    if (browser.enabled) {
        for (const auto& existing : insertionArray) {
            const JsonValue* item = ItemObject(existing);
            if (item && ToLowerInvariant(StringProperty(*item, L"Keyword")) ==
                    ToLowerInvariant(browser.prefix)) {
                result.message = L"Listary 关键字已被其他自定义项目占用：" + browser.prefix;
                return result;
            }
        }
        insertionArray.push_back(MakeInsertion(browser));
    }

    auto& updateArray = updates->array();
    const bool needsGoogleDisabled = HasOurGoogleInsertion(insertionArray);
    const bool googleAlreadyDisabled = HasDisabledGoogleUpdate(updateArray);
    bool googleDisabledByPlugin = ReadState(statePath);
    if (needsGoogleDisabled && !googleAlreadyDisabled) {
        AddDisabledGoogleUpdate(updateArray);
        googleDisabledByPlugin = true;
    } else if (!needsGoogleDisabled && googleDisabledByPlugin) {
        RemoveGoogleUpdate(updateArray);
        googleDisabledByPlugin = false;
    }

    const std::string serialized = SerializeJsonUtf8(root);
    JsonValue verification;
    std::wstring verificationError;
    if (!ParseJsonUtf8(serialized, verification, verificationError)) {
        result.message = L"生成的 Listary 配置校验失败：" + verificationError;
        return result;
    }
    if (!WriteAtomic(preferencesPath, serialized, result.backupPath, result.message)) return result;
    WriteState(statePath, googleDisabledByPlugin);
    WritePerBrowserMode(statePath);
    result.ok = true;
    result.message = browser.enabled ? L"当前浏览器已同步到 Listary。" : L"当前浏览器已从 Listary 移除。";
    return result;
}

ListaryConfigurationResult ListaryConfigurator::Remove(const std::filesystem::path& preferencesPath,
    const std::filesystem::path& statePath) {
    ListaryConfigurationResult result;
    result.preferencesPath = preferencesPath;
    if (preferencesPath.empty() || !std::filesystem::is_regular_file(preferencesPath)) {
        result.ok = true;
        result.message = L"未找到 Listary 配置，无需清理。";
        return result;
    }
    JsonValue root;
    if (!LoadDocument(preferencesPath, root, result.message)) return result;
    JsonValue* insertions = nullptr;
    JsonValue* updates = nullptr;
    if (!LocateWebSearch(root, insertions, updates, result.message)) return result;
    std::erase_if(insertions->array(), IsOurInsertion);
    if (ReadState(statePath)) RemoveGoogleUpdate(updates->array());
    const std::string serialized = SerializeJsonUtf8(root);
    if (!WriteAtomic(preferencesPath, serialized, result.backupPath, result.message)) return result;
    std::error_code ignored;
    std::filesystem::remove(statePath, ignored);
    result.ok = true;
    result.message = L"已移除本工具创建的 Listary 网页搜索项。";
    return result;
}
