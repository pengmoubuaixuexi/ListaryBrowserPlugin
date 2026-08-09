#include "browser_launcher.h"
#include "browser_registry.h"
#include "chromium_history_adapter.h"
#include "config_store.h"
#include "history_search_service.h"
#include "listary_configurator.h"
#include "listary_suggestion_server.h"
#include "query_parser.h"
#include "resource.h"
#include "snapshot_manager.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <Shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"BrowserHistoryLauncher.SearchWindow";
constexpr wchar_t kMutexName[] = L"Local\\BrowserHistoryLauncher.Singleton";
constexpr UINT kHotkeyId = 1;
constexpr UINT_PTR kDebounceTimer = 1;
constexpr UINT kTrayId = 1;
constexpr UINT kTrayMessage = WM_APP + 10;
constexpr UINT kShowMessage = WM_APP + 11;
constexpr UINT kResultsMessage = WM_APP + 12;
constexpr UINT kExitMessage = WM_APP + 13;
constexpr ULONG_PTR kQueryCopyData = 0x42484C51;
constexpr ULONG_PTR kListaryOpenCopyData = 0x42484C4F;
constexpr wchar_t kProtocolKey[] = L"Software\\Classes\\bhl";
constexpr int kEditId = 100;
constexpr int kListId = 101;
constexpr int kStatusId = 102;
constexpr int kMenuShow = 200;
constexpr int kMenuSettings = 201;
constexpr int kMenuExit = 202;

std::filesystem::path ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

bool SetRegistryString(HKEY key, const wchar_t* name, std::wstring_view value, std::wstring& error) {
    const std::wstring terminated(value);
    const auto bytes = static_cast<DWORD>((terminated.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = RegSetValueExW(key, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(terminated.c_str()), bytes);
    if (status == ERROR_SUCCESS) return true;
    error = L"无法写入 bhl:// 当前用户协议，错误码 " + std::to_wstring(status) + L"。";
    return false;
}

bool SetProtocolSubkey(std::wstring_view relativePath, std::wstring_view value, std::wstring& error) {
    const std::wstring path = std::wstring(kProtocolKey) + L"\\" + std::wstring(relativePath);
    HKEY key = nullptr;
    const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        error = L"无法创建 bhl:// 当前用户协议，错误码 " + std::to_wstring(status) + L"。";
        return false;
    }
    const bool ok = SetRegistryString(key, nullptr, value, error);
    RegCloseKey(key);
    return ok;
}

bool RegisterListaryProtocol(const std::filesystem::path& executable, std::wstring& error) {
    HKEY key = nullptr;
    const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kProtocolKey, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        error = L"无法创建 bhl:// 当前用户协议，错误码 " + std::to_wstring(status) + L"。";
        return false;
    }
    bool ok = SetRegistryString(key, nullptr, L"URL:Browser History Launcher", error) &&
        SetRegistryString(key, L"URL Protocol", L"", error);
    RegCloseKey(key);
    if (!ok) return false;
    if (!SetProtocolSubkey(L"DefaultIcon", L"\"" + executable.wstring() + L"\",0", error)) return false;
    const std::wstring command = L"\"" + executable.wstring() + L"\" --listary-open \"%1\"";
    return SetProtocolSubkey(L"shell\\open\\command", command, error);
}

bool UnregisterListaryProtocol(std::wstring& error) {
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, kProtocolKey);
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) return true;
    error = L"无法移除 bhl:// 当前用户协议，错误码 " + std::to_wstring(status) + L"。";
    return false;
}

class App {
public:
    App(HINSTANCE instance, AppConfig config, std::filesystem::path configPath)
        : instance_(instance), config_(std::move(config)), configPath_(std::move(configPath)),
          registry_(config_), adapter_(snapshots_),
          searchService_(adapter_, snapshots_, [this](SearchResponse&& response) {
              auto* payload = new SearchResponse(std::move(response));
              if (!window_ || !PostMessageW(window_, kResultsMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                  delete payload;
              }
          }), listaryServer_(config_) {}

    ~App() {
        RemoveTrayIcon();
        if (font_) DeleteObject(font_);
        if (smallFont_) DeleteObject(smallFont_);
    }

    bool Create(bool showImmediately) {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = &App::WindowProc;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON),
            IMAGE_ICON, 16, 16, LR_SHARED));
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass,
            L"浏览器历史启动器", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_BORDER,
            CW_USEDEFAULT, CW_USEDEFAULT, 780, 440, nullptr, nullptr, instance_, this);
        if (!window_) return false;
        if (config_.hotkeyVirtualKey != 0 &&
            !RegisterHotKey(window_, kHotkeyId, config_.hotkeyModifiers, config_.hotkeyVirtualKey)) {
            MessageBoxW(nullptr, L"无法注册全局快捷键，请修改 BrowserHistoryLauncher.ini 后重启。",
                L"浏览器历史启动器", MB_OK | MB_ICONERROR);
            DestroyWindow(window_);
            window_ = nullptr;
            return false;
        }
        AddTrayIcon();
        std::wstring listaryError;
        if (!listaryServer_.Start(listaryError)) {
            MessageBoxW(window_, listaryError.c_str(), L"Listary 建议服务未启动", MB_OK | MB_ICONWARNING);
        }
        if (showImmediately) Show();
        return true;
    }

    int Run() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

    void ShowWithQuery(std::wstring_view query) {
        Show();
        if (!edit_) return;
        SetWindowTextW(edit_, std::wstring(query).c_str());
        BeginSearch();
    }

    void OpenListaryUri(std::wstring_view uri) {
        std::wstring error;
        const auto item = listaryServer_.ResolveUri(uri, error);
        if (!item) {
            MessageBoxW(window_, error.c_str(), L"无法打开 Listary 历史结果", MB_OK | MB_ICONWARNING);
            return;
        }
        const auto* browser = registry_.FindById(item->browserId);
        if (!browser || !BrowserLauncher::OpenUrl(*browser, item->profileDirectory, item->url, error)) {
            if (error.empty()) error = L"结果来源浏览器配置已不可用。";
            MessageBoxW(window_, error.c_str(), L"无法打开 Listary 历史结果", MB_OK | MB_ICONERROR);
        }
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<App*>(create->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK EditSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR, DWORD_PTR data) {
        auto* app = reinterpret_cast<App*>(data);
        if (message == WM_KEYDOWN) {
            if (wParam == VK_ESCAPE) {
                app->Hide();
                return 0;
            }
            if (wParam == VK_DOWN || wParam == VK_UP) {
                app->MoveSelection(wParam == VK_DOWN ? 1 : -1);
                return 0;
            }
            if (wParam == VK_RETURN) {
                app->OpenSelection();
                return 0;
            }
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: return OnCreate() ? 0 : -1;
        case WM_SIZE: Layout(LOWORD(lParam), HIWORD(lParam)); return 0;
        case WM_HOTKEY: if (wParam == kHotkeyId) Show(); return 0;
        case WM_TIMER: if (wParam == kDebounceTimer) BeginSearch(); return 0;
        case WM_COMMAND: return OnCommand(LOWORD(wParam), HIWORD(wParam));
        case WM_NOTIFY: return OnNotify(reinterpret_cast<NMHDR*>(lParam));
        case WM_COPYDATA: return OnCopyData(reinterpret_cast<const COPYDATASTRUCT*>(lParam));
        case WM_CLOSE: Hide(); return 0;
        case kTrayMessage: OnTrayMessage(lParam); return 0;
        case kShowMessage: Show(); return 0;
        case kExitMessage: DestroyWindow(window_); return 0;
        case kResultsMessage: ApplyResults(reinterpret_cast<SearchResponse*>(lParam)); return 0;
        case WM_DESTROY:
            if (config_.hotkeyVirtualKey != 0) UnregisterHotKey(window_, kHotkeyId);
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    LRESULT OnCopyData(const COPYDATASTRUCT* data) {
        if (!data || (data->dwData != kQueryCopyData && data->dwData != kListaryOpenCopyData) ||
            !data->lpData || data->cbData < sizeof(wchar_t) ||
            data->cbData > 32768 * sizeof(wchar_t) || data->cbData % sizeof(wchar_t) != 0) {
            return FALSE;
        }
        const auto count = static_cast<std::size_t>(data->cbData / sizeof(wchar_t));
        const auto* text = static_cast<const wchar_t*>(data->lpData);
        if (text[count - 1] != L'\0') return FALSE;
        const std::wstring_view value(text, count - 1);
        if (data->dwData == kQueryCopyData) ShowWithQuery(value);
        else OpenListaryUri(value);
        return TRUE;
    }

    bool CreateSearchControls() {
        if (edit_ && status_ && list_) return true;
        edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), instance_, nullptr);
        status_ = CreateWindowExW(0, L"STATIC", L"输入 g 或 e，加空格与关键词",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusId)), instance_, nullptr);
        list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), instance_, nullptr);
        if (!edit_ || !status_ || !list_) return false;
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        SetWindowSubclass(edit_, EditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(L"标题"); column.cx = 280; column.iSubItem = 0;
        ListView_InsertColumn(list_, 0, &column);
        column.pszText = const_cast<wchar_t*>(L"URL"); column.cx = 350; column.iSubItem = 1;
        ListView_InsertColumn(list_, 1, &column);
        column.pszText = const_cast<wchar_t*>(L"来源"); column.cx = 110; column.iSubItem = 2;
        ListView_InsertColumn(list_, 2, &column);
        RECT client{};
        GetClientRect(window_, &client);
        Layout(client.right, client.bottom);
        return true;
    }

    bool OnCreate() {
        font_ = CreateFontW(-22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        smallFont_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        return font_ != nullptr && smallFont_ != nullptr;
    }

    void Layout(int width, int height) {
        constexpr int margin = 12;
        constexpr int editHeight = 38;
        constexpr int statusHeight = 24;
        MoveWindow(edit_, margin, margin, std::max(0, width - margin * 2), editHeight, TRUE);
        MoveWindow(status_, margin, margin + editHeight + 7, std::max(0, width - margin * 2), statusHeight, TRUE);
        MoveWindow(list_, margin, margin + editHeight + statusHeight + 9,
            std::max(0, width - margin * 2), std::max(0, height - editHeight - statusHeight - margin * 2 - 9), TRUE);
    }

    LRESULT OnCommand(int id, int notification) {
        if (id == kEditId && notification == EN_CHANGE && IsWindowVisible(window_)) {
            KillTimer(window_, kDebounceTimer);
            SetTimer(window_, kDebounceTimer, config_.debounceMs, nullptr);
            return 0;
        }
        if (id == kMenuShow) Show();
        else if (id == kMenuSettings) OpenSettings();
        else if (id == kMenuExit) DestroyWindow(window_);
        return 0;
    }

    LRESULT OnNotify(NMHDR* header) {
        if (header && header->hwndFrom == list_ && header->code == NM_DBLCLK) {
            OpenSelection();
            return 0;
        }
        return 0;
    }

    void Show() {
        if (!CreateSearchControls()) {
            MessageBoxW(window_, L"无法创建搜索控件。", L"浏览器历史启动器", MB_OK | MB_ICONERROR);
            return;
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor, &info);
        RECT rect{};
        GetWindowRect(window_, &rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
        const int y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 3;
        SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
        SetForegroundWindow(window_);
        SetFocus(edit_);
        SendMessageW(edit_, EM_SETSEL, 0, -1);
    }

    void Hide() {
        KillTimer(window_, kDebounceTimer);
        ShowWindow(window_, SW_HIDE);
        ++generation_;
        searchService_.CancelAndCleanup(generation_);
        results_.clear();
        results_.shrink_to_fit();
        ListView_DeleteAllItems(list_);
        SetWindowTextW(edit_, L"");
        SetWindowTextW(status_, L"输入 g 或 e，加空格与关键词");
    }

    void BeginSearch() {
        KillTimer(window_, kDebounceTimer);
        const auto parsed = ParseQuery(WindowText(edit_));
        if (parsed.empty) {
            ++generation_;
            searchService_.CancelAndCleanup(generation_);
            results_.clear();
            ListView_DeleteAllItems(list_);
            SetWindowTextW(status_, L"输入 g 或 e，加空格与关键词");
            return;
        }
        const auto* browser = registry_.FindByPrefix(parsed.prefix);
        if (!browser) {
            ++generation_;
            searchService_.CancelAndCleanup(generation_);
            results_.clear();
            ListView_DeleteAllItems(list_);
            SetWindowTextW(status_, L"未知或未启用的浏览器前缀");
            return;
        }
        results_.clear();
        ListView_DeleteAllItems(list_);
        SetWindowTextW(status_, (L"正在查询 " + browser->name + L"…").c_str());
        searchService_.Submit(*browser, parsed.query, config_.maxResults, ++generation_);
    }

    void ApplyResults(SearchResponse* rawResponse) {
        std::unique_ptr<SearchResponse> response(rawResponse);
        if (!response || response->generation != generation_ || !IsWindowVisible(window_)) return;
        results_ = std::move(response->results);
        ListView_DeleteAllItems(list_);
        for (std::size_t i = 0; i < results_.size(); ++i) {
            const auto& item = results_[i];
            LVITEMW row{};
            row.mask = LVIF_TEXT;
            row.iItem = static_cast<int>(i);
            row.pszText = const_cast<wchar_t*>(item.title.c_str());
            ListView_InsertItem(list_, &row);
            ListView_SetItemText(list_, static_cast<int>(i), 1, const_cast<wchar_t*>(item.url.c_str()));
            const std::wstring source = item.browserName + (item.profileDirectory.empty() ? L"" : L":" + item.profileDirectory);
            ListView_SetItemText(list_, static_cast<int>(i), 2, const_cast<wchar_t*>(source.c_str()));
        }
        if (!results_.empty()) {
            ListView_SetItemState(list_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        std::wstring status;
        if (!response->error.empty() && results_.empty()) status = response->error;
        else status = std::to_wstring(results_.size()) + L" 条结果 · " +
            std::to_wstring(response->elapsedMicroseconds / 1000) + L" ms";
        SetWindowTextW(status_, status.c_str());
    }

    void MoveSelection(int direction) {
        if (results_.empty()) return;
        int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (selected < 0) selected = direction > 0 ? 0 : static_cast<int>(results_.size()) - 1;
        else selected = std::clamp(selected + direction, 0, static_cast<int>(results_.size()) - 1);
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list_, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, selected, FALSE);
    }

    void OpenSelection() {
        int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (selected < 0 && !results_.empty()) selected = 0;
        if (selected < 0 || static_cast<std::size_t>(selected) >= results_.size()) return;
        const auto& item = results_[static_cast<std::size_t>(selected)];
        const auto* browser = registry_.FindById(item.browserId);
        if (!browser) {
            SetWindowTextW(status_, L"结果来源浏览器配置已不可用。");
            return;
        }
        std::wstring error;
        if (!BrowserLauncher::OpenUrl(*browser, item.profileDirectory, item.url, error)) {
            SetWindowTextW(status_, error.c_str());
            return;
        }
        Hide();
    }

    void AddTrayIcon() {
        if (trayAdded_) return;
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = kTrayId;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray_.uCallbackMessage = kTrayMessage;
        tray_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        wcscpy_s(tray_.szTip, L"浏览器历史启动器");
        trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
    }

    void RemoveTrayIcon() {
        if (trayAdded_) {
            Shell_NotifyIconW(NIM_DELETE, &tray_);
            trayAdded_ = false;
        }
    }

    void OnTrayMessage(LPARAM event) {
        if (event == WM_LBUTTONDBLCLK) {
            Show();
            return;
        }
        if (event != WM_RBUTTONUP && event != WM_CONTEXTMENU) return;
        POINT cursor{};
        GetCursorPos(&cursor);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuShow, L"显示");
        AppendMenuW(menu, MF_STRING, kMenuSettings, L"设置");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void OpenSettings() {
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", configPath_.c_str(),
            nullptr, configPath_.parent_path().c_str(), SW_SHOWNORMAL));
        if (result <= 32) {
            MessageBoxW(window_, L"无法打开配置文件。", L"浏览器历史启动器", MB_OK | MB_ICONERROR);
        }
    }

    HINSTANCE instance_{};
    HWND window_{};
    HWND edit_{};
    HWND status_{};
    HWND list_{};
    HFONT font_{};
    HFONT smallFont_{};
    NOTIFYICONDATAW tray_{};
    bool trayAdded_ = false;
    AppConfig config_;
    std::filesystem::path configPath_;
    BrowserRegistry registry_;
    SnapshotManager snapshots_;
    ChromiumHistoryAdapter adapter_;
    HistorySearchService searchService_;
    ListarySuggestionServer listaryServer_;
    std::vector<HistoryResult> results_;
    std::uint64_t generation_ = 0;
};

bool HasArgument(std::wstring_view argument) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool found = false;
    if (arguments) {
        for (int i = 1; i < count; ++i) {
            if (CompareStringOrdinal(arguments[i], -1, argument.data(), static_cast<int>(argument.size()), TRUE) == CSTR_EQUAL) {
                found = true;
                break;
            }
        }
        LocalFree(arguments);
    }
    return found;
}

std::optional<std::wstring> ArgumentValue(std::wstring_view argument) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::optional<std::wstring> value;
    if (arguments) {
        for (int i = 1; i + 1 < count; ++i) {
            if (CompareStringOrdinal(arguments[i], -1, argument.data(), static_cast<int>(argument.size()), TRUE) == CSTR_EQUAL) {
                value = arguments[i + 1];
                break;
            }
        }
        LocalFree(arguments);
    }
    return value;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    const auto requestedQuery = ArgumentValue(L"--query");
    const auto listaryOpenUri = ArgumentValue(L"--listary-open");
    if (HasArgument(L"--configure-listary") || HasArgument(L"--remove-listary-integration")) {
        const bool quiet = HasArgument(L"--quiet");
        if (ListaryConfigurator::IsListaryRunning()) {
            if (!quiet) {
                MessageBoxW(nullptr, L"请先从托盘完全退出 Listary，然后重试。",
                    L"Listary 浏览器插件", MB_OK | MB_ICONWARNING);
            }
            return 6;
        }
        const auto directory = ExecutableDirectory();
        const auto preferences = ListaryConfigurator::DetectPreferences();
        const auto statePath = directory / L"ListaryIntegrationState.ini";
        ListaryConfigurationResult result;
        if (HasArgument(L"--configure-listary")) {
            std::wstring warning;
            const auto config = ConfigStore::Load(directory / L"BrowserHistoryLauncher.ini", warning);
            std::wstring validationError;
            if (!ConfigStore::Validate(config, validationError)) {
                result.message = validationError;
            } else {
                result = ListaryConfigurator::Configure(config, preferences, statePath);
            }
        } else {
            result = ListaryConfigurator::Remove(preferences, statePath);
        }
        if (!quiet) {
            MessageBoxW(nullptr, result.message.c_str(), L"Listary 浏览器插件",
                MB_OK | (result.ok ? MB_ICONINFORMATION : MB_ICONERROR));
        }
        return result.ok ? 0 : (preferences.empty() ? 5 : 7);
    }
    if (HasArgument(L"--register-listary-protocol") || HasArgument(L"--unregister-listary-protocol")) {
        std::wstring error;
        const bool registering = HasArgument(L"--register-listary-protocol");
        const bool ok = registering ? RegisterListaryProtocol(ExecutableDirectory() / L"BrowserHistoryLauncher.exe", error) :
                                      UnregisterListaryProtocol(error);
        if (!ok || !HasArgument(L"--quiet")) {
            const std::wstring message = ok ?
                (registering ? L"已为当前用户注册 bhl:// 协议。" : L"已移除当前用户的 bhl:// 协议。") : error;
            MessageBoxW(nullptr, message.c_str(), L"Browser History Launcher", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        }
        return ok ? 0 : 4;
    }
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!mutex) return 1;
    const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
    if (alreadyRunning) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            if (HasArgument(L"--exit")) {
                PostMessageW(existing, kExitMessage, 0, 0);
            } else if (listaryOpenUri) {
                COPYDATASTRUCT data{};
                data.dwData = kListaryOpenCopyData;
                data.cbData = static_cast<DWORD>((listaryOpenUri->size() + 1) * sizeof(wchar_t));
                data.lpData = const_cast<wchar_t*>(listaryOpenUri->c_str());
                DWORD_PTR ignored = 0;
                SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &ignored);
            } else if (requestedQuery) {
                COPYDATASTRUCT data{};
                data.dwData = kQueryCopyData;
                data.cbData = static_cast<DWORD>((requestedQuery->size() + 1) * sizeof(wchar_t));
                data.lpData = const_cast<wchar_t*>(requestedQuery->c_str());
                DWORD_PTR ignored = 0;
                SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &ignored);
            } else {
                PostMessageW(existing, kShowMessage, 0, 0);
            }
        }
        CloseHandle(mutex);
        return 0;
    }
    if (HasArgument(L"--exit")) {
        CloseHandle(mutex);
        return 0;
    }

    const auto configPath = ExecutableDirectory() / L"BrowserHistoryLauncher.ini";
    std::wstring warning;
    AppConfig config = ConfigStore::Load(configPath, warning);
    std::wstring validationError;
    if (!ConfigStore::Validate(config, validationError)) {
        MessageBoxW(nullptr, validationError.c_str(), L"浏览器历史启动器配置错误", MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 2;
    }

    App app(instance, std::move(config), configPath);
    if (!app.Create(HasArgument(L"--show") || requestedQuery.has_value())) {
        CloseHandle(mutex);
        return 3;
    }
    if (requestedQuery) app.ShowWithQuery(*requestedQuery);
    if (listaryOpenUri) app.OpenListaryUri(*listaryOpenUri);
    const int result = app.Run();
    CloseHandle(mutex);
    return result;
}
