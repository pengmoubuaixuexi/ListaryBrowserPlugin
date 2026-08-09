#include "listary_settings_dialog.h"

#include "browser_launcher.h"
#include "resource.h"
#include "text_util.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kClassName[] = L"BrowserHistoryLauncher.ListarySettings";
constexpr int kBrowserId = 1001;
constexpr int kEnabledId = 1002;
constexpr int kKeywordId = 1003;
constexpr int kIconId = 1004;
constexpr int kBrowseId = 1005;
constexpr int kDetectedPathId = 1006;
constexpr int kApplyId = IDOK;

std::wstring ControlText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

bool ValidKeyword(std::wstring_view keyword) {
    if (keyword.empty() || keyword.size() > 16) return false;
    return std::all_of(keyword.begin(), keyword.end(), [](wchar_t ch) {
        return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_';
    });
}

class DialogState {
public:
    DialogState(HINSTANCE instance, HWND owner, const AppConfig& config)
        : instance_(instance), owner_(owner) {
        for (const auto& browser : config.browsers) {
            const auto executable = BrowserLauncher::FindExecutable(browser);
            if (executable.empty() && !browser.enabled) continue;
            browsers_.push_back(browser);
            executables_.push_back(executable);
        }
    }

    std::optional<BrowserDefinition> Run() {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        windowClass.lpszClassName = kClassName;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return std::nullopt;

        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"配置 Listary 浏览器插件",
            WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 600, 356,
            owner_, nullptr, instance_, this);
        if (!window_) return std::nullopt;

        RECT ownerRect{};
        RECT dialogRect{};
        GetWindowRect(owner_, &ownerRect);
        GetWindowRect(window_, &dialogRect);
        const int width = dialogRect.right - dialogRect.left;
        const int height = dialogRect.bottom - dialogRect.top;
        SetWindowPos(window_, HWND_TOP,
            ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2,
            ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2,
            0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
        EnableWindow(owner_, FALSE);

        MSG message{};
        while (IsWindow(window_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        EnableWindow(owner_, TRUE);
        SetActiveWindow(owner_);
        return result_;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            state = static_cast<DialogState*>(create->lpCreateParams);
            state->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
        return state ? state->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    HWND AddControl(const wchar_t* className, const wchar_t* text, DWORD style,
        int x, int y, int width, int height, int id = 0, DWORD extendedStyle = 0) {
        HWND control = CreateWindowExW(extendedStyle, className, text, WS_CHILD | WS_VISIBLE | style,
            x, y, width, height, window_, id ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
            instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return control;
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: return CreateControls() ? 0 : -1;
        case WM_COMMAND: return OnCommand(LOWORD(wParam), HIWORD(wParam));
        case WM_CLOSE: DestroyWindow(window_); return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    bool CreateControls() {
        AddControl(L"STATIC", L"选择浏览器", SS_LEFT, 24, 24, 110, 22);
        browserCombo_ = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
            145, 20, 410, 220, kBrowserId, WS_EX_CLIENTEDGE);
        AddControl(L"STATIC", L"每次打开都会重新检测 BrowserHistoryLauncher.ini 中定义的浏览器。",
            SS_LEFT, 145, 52, 410, 22);

        enabledCheck_ = AddControl(L"BUTTON", L"启用该浏览器，并添加到 Listary 下拉结果",
            BS_AUTOCHECKBOX | WS_TABSTOP, 145, 82, 410, 24, kEnabledId);
        AddControl(L"STATIC", L"Listary 关键字", SS_LEFT, 24, 124, 110, 22);
        keywordEdit_ = AddControl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
            145, 120, 140, 26, kKeywordId, WS_EX_CLIENTEDGE);
        AddControl(L"STATIC", L"例如 g、e、br；允许字母、数字、- 和 _。", SS_LEFT, 298, 124, 260, 22);

        AddControl(L"STATIC", L"图标位置", SS_LEFT, 24, 164, 110, 22);
        iconEdit_ = AddControl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
            145, 160, 320, 26, kIconId, WS_EX_CLIENTEDGE);
        AddControl(L"BUTTON", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP,
            475, 159, 80, 28, kBrowseId);

        AddControl(L"STATIC", L"检测路径", SS_LEFT, 24, 204, 110, 22);
        pathLabel_ = AddControl(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS,
            145, 204, 410, 40, kDetectedPathId);
        AddControl(L"STATIC", L"应用时会更新本项目配置、当前用户 bhl:// 注册表和 Listary 配置。",
            SS_LEFT, 24, 258, 530, 22);

        applyButton_ = AddControl(L"BUTTON", L"应用", BS_DEFPUSHBUTTON | WS_TABSTOP,
            377, 289, 85, 30, kApplyId);
        AddControl(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP,
            470, 289, 85, 30, IDCANCEL);

        if (!browserCombo_ || !enabledCheck_ || !keywordEdit_ || !iconEdit_ || !pathLabel_ || !applyButton_) {
            return false;
        }
        for (std::size_t i = 0; i < browsers_.size(); ++i) {
            std::wstring label = browsers_[i].name;
            if (executables_[i].empty()) label += L"（当前未检测到，可停用）";
            const LRESULT item = SendMessageW(browserCombo_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(label.c_str()));
            if (item >= 0) SendMessageW(browserCombo_, CB_SETITEMDATA, item, static_cast<LPARAM>(i));
        }
        if (browsers_.empty()) {
            SendMessageW(browserCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"未检测到受支持的浏览器"));
            EnableWindow(browserCombo_, FALSE);
            EnableWindow(applyButton_, FALSE);
            SetWindowTextW(pathLabel_, L"可在 BrowserHistoryLauncher.ini 中添加 browser.* 定义后重新打开。");
        } else {
            SendMessageW(browserCombo_, CB_SETCURSEL, 0, 0);
            SelectBrowser();
        }
        SetFocus(browserCombo_);
        return true;
    }

    void SelectBrowser() {
        const LRESULT selected = SendMessageW(browserCombo_, CB_GETCURSEL, 0, 0);
        if (selected == CB_ERR) return;
        const auto index = static_cast<std::size_t>(SendMessageW(browserCombo_, CB_GETITEMDATA, selected, 0));
        if (index >= browsers_.size()) return;
        selectedIndex_ = index;
        const auto& browser = browsers_[index];
        Button_SetCheck(enabledCheck_, browser.enabled ? BST_CHECKED : BST_UNCHECKED);
        SetWindowTextW(keywordEdit_, browser.prefix.c_str());
        const auto icon = browser.iconPath.empty() ? executables_[index] : browser.iconPath;
        SetWindowTextW(iconEdit_, icon.c_str());
        SetWindowTextW(pathLabel_, executables_[index].empty() ? L"未找到可执行文件" : executables_[index].c_str());
    }

    void BrowseIcon() {
        std::wstring buffer(32768, L'\0');
        const std::wstring current = ControlText(iconEdit_);
        std::copy_n(current.c_str(), std::min(current.size(), buffer.size() - 1), buffer.data());
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"图标或程序 (*.ico;*.exe)\0*.ico;*.exe\0所有文件 (*.*)\0*.*\0\0";
        dialog.lpstrFile = buffer.data();
        dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        if (GetOpenFileNameW(&dialog)) SetWindowTextW(iconEdit_, buffer.c_str());
    }

    void Apply() {
        if (selectedIndex_ >= browsers_.size()) return;
        BrowserDefinition browser = browsers_[selectedIndex_];
        browser.enabled = Button_GetCheck(enabledCheck_) == BST_CHECKED;
        browser.prefix = ToLowerInvariant(Trim(ControlText(keywordEdit_)));
        browser.iconPath = Trim(ControlText(iconEdit_));
        if (!ValidKeyword(browser.prefix)) {
            MessageBoxW(window_, L"关键字应为 1–16 个字母、数字、- 或 _。", L"配置 Listary", MB_OK | MB_ICONWARNING);
            SetFocus(keywordEdit_);
            return;
        }
        if (browser.enabled && executables_[selectedIndex_].empty()) {
            MessageBoxW(window_, L"没有检测到该浏览器的可执行文件，不能启用。", L"配置 Listary", MB_OK | MB_ICONWARNING);
            return;
        }
        if (!browser.iconPath.empty()) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(browser.iconPath, error)) {
                MessageBoxW(window_, L"图标位置不是现有文件。可选择 .ico 或浏览器 .exe。",
                    L"配置 Listary", MB_OK | MB_ICONWARNING);
                SetFocus(iconEdit_);
                return;
            }
        }
        result_ = std::move(browser);
        DestroyWindow(window_);
    }

    LRESULT OnCommand(int id, int notification) {
        if (id == kBrowserId && notification == CBN_SELCHANGE) SelectBrowser();
        else if (id == kBrowseId && notification == BN_CLICKED) BrowseIcon();
        else if (id == kApplyId && notification == BN_CLICKED) Apply();
        else if (id == IDCANCEL && notification == BN_CLICKED) DestroyWindow(window_);
        return 0;
    }

    HINSTANCE instance_{};
    HWND owner_{};
    HWND window_{};
    HWND browserCombo_{};
    HWND enabledCheck_{};
    HWND keywordEdit_{};
    HWND iconEdit_{};
    HWND pathLabel_{};
    HWND applyButton_{};
    std::vector<BrowserDefinition> browsers_;
    std::vector<std::filesystem::path> executables_;
    std::size_t selectedIndex_ = static_cast<std::size_t>(-1);
    std::optional<BrowserDefinition> result_;
};
}

std::optional<BrowserDefinition> ListarySettingsDialog::Show(HINSTANCE instance, HWND owner,
    const AppConfig& config) {
    DialogState dialog(instance, owner, config);
    return dialog.Run();
}
