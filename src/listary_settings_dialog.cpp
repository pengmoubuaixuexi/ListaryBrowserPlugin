#include "listary_settings_dialog.h"

#include "browser_icon.h"
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
constexpr int kBluetoothEnabledId = 1101;
constexpr int kBluetoothKeywordId = 1102;
constexpr int kApplyId = IDOK;

constexpr int kClientWidth = 680;
constexpr int kClientHeight = 611;

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
    DialogState(HINSTANCE instance, HWND owner, const AppConfig& config,
        std::wstring_view)
        : instance_(instance), owner_(owner), bluetooth_(config.bluetooth) {
        for (const auto& browser : config.browsers) {
            const auto executable = BrowserLauncher::FindExecutable(browser);
            if (executable.empty() && !browser.enabled) continue;
            browsers_.push_back(browser);
            executables_.push_back(executable);
        }
    }

    ~DialogState() {
        DeleteFonts();
    }

    std::optional<ListarySettingsResult> Run() {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        windowClass.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        windowClass.lpszClassName = kClassName;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return std::nullopt;
        }

        dpi_ = owner_ && IsWindow(owner_) ? GetDpiForWindow(owner_) : GetDpiForSystem();
        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        const DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
        // Use client coordinates as reported to this window. Some Windows DPI
        // configurations virtualize these coordinates even though GetDpiForWindow
        // reports the monitor's physical DPI.
        RECT windowRect{0, 0, kClientWidth, kClientHeight};
        AdjustWindowRectExForDpi(&windowRect, style, FALSE, extendedStyle, dpi_);

        window_ = CreateWindowExW(extendedStyle, kClassName, L"配置 Listary 插件",
            style, CW_USEDEFAULT, CW_USEDEFAULT,
            windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
            owner_, nullptr, instance_, this);
        if (!window_) return std::nullopt;

        dpi_ = GetDpiForWindow(window_);
        CreateFonts();
        LayoutControls();
        CenterAndShow();
        if (owner_ && IsWindow(owner_)) EnableWindow(owner_, FALSE);

        MSG message{};
        while (IsWindow(window_)) {
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) break;
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (owner_ && IsWindow(owner_)) {
            EnableWindow(owner_, TRUE);
            SetActiveWindow(owner_);
        }
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
        return state ? state->HandleMessage(message, wParam, lParam) :
            DefWindowProcW(window, message, wParam, lParam);
    }

    HWND AddControl(const wchar_t* className, const wchar_t* text, DWORD style,
        int id = 0, DWORD extendedStyle = 0) {
        HWND control = CreateWindowExW(extendedStyle, className, text,
            WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window_,
            id ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
            instance_, nullptr);
        if (control) {
            controls_.push_back(control);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        }
        return control;
    }

    void Move(HWND control, int x, int y, int width, int height) const {
        if (!control) return;
        RECT clientRect{};
        GetClientRect(window_, &clientRect);
        const int clientWidth = std::max(1L, clientRect.right - clientRect.left);
        const int clientHeight = std::max(1L, clientRect.bottom - clientRect.top);
        const int left = MulDiv(x, clientWidth, kClientWidth);
        const int top = MulDiv(y, clientHeight, kClientHeight);
        const int right = MulDiv(x + width, clientWidth, kClientWidth);
        const int bottom = MulDiv(y + height, clientHeight, kClientHeight);
        MoveWindow(control, left, top, right - left, bottom - top, TRUE);
    }

    HFONT MakeFont(int pointSize, int weight) const {
        RECT clientRect{};
        GetClientRect(window_, &clientRect);
        const int clientWidth = std::max(1L, clientRect.right - clientRect.left);
        const int clientHeight = std::max(1L, clientRect.bottom - clientRect.top);
        const int horizontalDpi = MulDiv(clientWidth, 96, kClientWidth);
        const int verticalDpi = MulDiv(clientHeight, 96, kClientHeight);
        const int layoutDpi = std::clamp(std::min(horizontalDpi, verticalDpi), 72, 384);
        return CreateFontW(-MulDiv(pointSize, layoutDpi, 72), 0, 0, 0, weight,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }

    void DeleteFonts() {
        if (titleFont_) DeleteObject(titleFont_);
        if (bodyFont_) DeleteObject(bodyFont_);
        if (sectionFont_) DeleteObject(sectionFont_);
        if (smallFont_) DeleteObject(smallFont_);
        titleFont_ = nullptr;
        bodyFont_ = nullptr;
        sectionFont_ = nullptr;
        smallFont_ = nullptr;
    }

    void CreateFonts() {
        DeleteFonts();
        bodyFont_ = MakeFont(10, FW_NORMAL);
        smallFont_ = MakeFont(9, FW_NORMAL);
        sectionFont_ = MakeFont(10, FW_SEMIBOLD);
        titleFont_ = MakeFont(16, FW_SEMIBOLD);

        for (HWND control : controls_) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        }
        if (titleLabel_) SendMessageW(titleLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
        if (browserSection_) SendMessageW(browserSection_, WM_SETFONT, reinterpret_cast<WPARAM>(sectionFont_), TRUE);
        if (listarySection_) SendMessageW(listarySection_, WM_SETFONT, reinterpret_cast<WPARAM>(sectionFont_), TRUE);
        if (bluetoothSection_) SendMessageW(bluetoothSection_, WM_SETFONT, reinterpret_cast<WPARAM>(sectionFont_), TRUE);
        for (HWND control : mutedControls_) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        }
    }

    void LayoutControls() const {
        Move(titleLabel_, 28, 20, 624, 31);
        Move(subtitleLabel_, 28, 56, 624, 34);
        Move(topSeparator_, 28, 100, 624, 2);

        Move(browserSection_, 28, 116, 120, 24);
        Move(browserLabel_, 28, 149, 106, 26);
        Move(browserCombo_, 150, 144, 502, 230);
        Move(browserSummary_, 150, 180, 502, 20);
        Move(pathTitle_, 28, 207, 106, 24);
        Move(pathLabel_, 150, 204, 502, 28);
        Move(middleSeparator_, 28, 244, 624, 2);

        Move(listarySection_, 28, 260, 150, 24);
        Move(enabledCheck_, 150, 286, 502, 28);
        Move(keywordLabel_, 28, 328, 106, 26);
        Move(keywordEdit_, 150, 322, 158, 31);
        Move(keywordHint_, 324, 328, 328, 24);
        Move(iconLabel_, 28, 371, 106, 26);
        Move(iconEdit_, 150, 365, 390, 31);
        Move(browseButton_, 550, 364, 102, 33);
        Move(updateHint_, 28, 405, 624, 30);
        Move(bluetoothSeparator_, 28, 442, 624, 2);

        Move(bluetoothSection_, 28, 456, 180, 24);
        Move(bluetoothEnabledCheck_, 150, 482, 502, 28);
        Move(bluetoothKeywordLabel_, 28, 521, 106, 26);
        Move(bluetoothKeywordEdit_, 150, 515, 158, 31);
        Move(bluetoothKeywordHint_, 324, 521, 328, 24);
        Move(bottomSeparator_, 28, 563, 624, 2);
        Move(applyButton_, 440, 576, 112, 32);
        Move(cancelButton_, 562, 576, 90, 32);
    }

    void CenterAndShow() const {
        RECT dialogRect{};
        GetWindowRect(window_, &dialogRect);
        const int width = dialogRect.right - dialogRect.left;
        const int height = dialogRect.bottom - dialogRect.top;

        RECT anchor{};
        if (owner_ && IsWindow(owner_)) {
            GetWindowRect(owner_, &anchor);
        } else {
            MONITORINFO monitorInfo{sizeof(monitorInfo)};
            GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY), &monitorInfo);
            anchor = monitorInfo.rcWork;
        }

        const HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        GetMonitorInfoW(monitor, &monitorInfo);
        const RECT& work = monitorInfo.rcWork;
        int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
        int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
        x = std::clamp(x, static_cast<int>(work.left),
            std::max(static_cast<int>(work.left), static_cast<int>(work.right) - width));
        y = std::clamp(y, static_cast<int>(work.top),
            std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - height));
        SetWindowPos(window_, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            return CreateControls() ? 0 : -1;
        case WM_COMMAND:
            return OnCommand(LOWORD(wParam), HIWORD(wParam));
        case WM_SIZE:
            if (!controls_.empty()) {
                CreateFonts();
                LayoutControls();
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            return OnStaticColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));
        case WM_DPICHANGED:
            OnDpiChanged(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
            return 0;
        case WM_CLOSE:
            DestroyWindow(window_);
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    bool CreateControls() {
        CreateFonts();
        titleLabel_ = AddControl(L"STATIC", L"配置 Listary 插件", SS_LEFT);
        subtitleLabel_ = AddControl(L"STATIC",
            L"可配置浏览器搜索和蓝牙设备；最后统一保存并只重启一次 Listary。",
            SS_LEFT);
        topSeparator_ = AddControl(L"STATIC", L"", SS_ETCHEDHORZ);

        browserSection_ = AddControl(L"STATIC", L"浏览器", SS_LEFT);
        browserLabel_ = AddControl(L"STATIC", L"选择浏览器", SS_LEFT);
        browserCombo_ = AddControl(WC_COMBOBOXW, L"",
            CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kBrowserId, WS_EX_CLIENTEDGE);
        browserSummary_ = AddControl(L"STATIC", L"", SS_LEFT);
        pathTitle_ = AddControl(L"STATIC", L"程序位置", SS_LEFT);
        pathLabel_ = AddControl(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, kDetectedPathId);
        middleSeparator_ = AddControl(L"STATIC", L"", SS_ETCHEDHORZ);

        listarySection_ = AddControl(L"STATIC", L"Listary 搜索", SS_LEFT);
        enabledCheck_ = AddControl(L"BUTTON", L"在 Listary 中启用这个浏览器",
            BS_AUTOCHECKBOX | WS_TABSTOP, kEnabledId);
        keywordLabel_ = AddControl(L"STATIC", L"搜索关键字", SS_LEFT);
        keywordEdit_ = AddControl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
            kKeywordId, WS_EX_CLIENTEDGE);
        keywordHint_ = AddControl(L"STATIC", L"1–16 位字母、数字、- 或 _，例如 g、e、br", SS_LEFT);
        iconLabel_ = AddControl(L"STATIC", L"结果图标", SS_LEFT);
        iconEdit_ = AddControl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
            kIconId, WS_EX_CLIENTEDGE);
        browseButton_ = AddControl(L"BUTTON", L"选择文件…", BS_PUSHBUTTON | WS_TABSTOP, kBrowseId);
        updateHint_ = AddControl(L"STATIC",
            L"切换下拉框可继续配置其他浏览器，点击“保存并应用全部”后统一同步。这里设置的是 Listary 搜索关键字，不是唤醒快捷键。",
            SS_LEFT);
        bluetoothSeparator_ = AddControl(L"STATIC", L"", SS_ETCHEDHORZ);

        bluetoothSection_ = AddControl(L"STATIC", L"蓝牙设备", SS_LEFT);
        bluetoothEnabledCheck_ = AddControl(L"BUTTON", L"在 Listary 中启用蓝牙设备列表和连接",
            BS_AUTOCHECKBOX | WS_TABSTOP, kBluetoothEnabledId);
        bluetoothKeywordLabel_ = AddControl(L"STATIC", L"搜索关键字", SS_LEFT);
        bluetoothKeywordEdit_ = AddControl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
            kBluetoothKeywordId, WS_EX_CLIENTEDGE);
        bluetoothKeywordHint_ = AddControl(L"STATIC", L"例如 ly；不能与浏览器关键字重复", SS_LEFT);
        bottomSeparator_ = AddControl(L"STATIC", L"", SS_ETCHEDHORZ);

        applyButton_ = AddControl(L"BUTTON", L"保存并应用全部", BS_DEFPUSHBUTTON | WS_TABSTOP, kApplyId);
        cancelButton_ = AddControl(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);

        if (!titleLabel_ || !subtitleLabel_ || !browserCombo_ || !enabledCheck_ ||
            !keywordEdit_ || !iconEdit_ || !pathLabel_ || !bluetoothEnabledCheck_ ||
            !bluetoothKeywordEdit_ || !applyButton_ || !cancelButton_) {
            return false;
        }

        mutedControls_ = {subtitleLabel_, browserSummary_, pathLabel_, keywordHint_, updateHint_,
            bluetoothKeywordHint_};
        CreateFonts();
        LayoutControls();
        SendMessageW(browserCombo_, CB_SETMINVISIBLE, 8, 0);

        std::size_t detectedCount = 0;
        for (std::size_t i = 0; i < browsers_.size(); ++i) {
            std::wstring label = browsers_[i].name;
            if (executables_[i].empty()) {
                label += L"（当前未检测到）";
            } else {
                ++detectedCount;
            }
            const LRESULT item = SendMessageW(browserCombo_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(label.c_str()));
            if (item >= 0) SendMessageW(browserCombo_, CB_SETITEMDATA, item, static_cast<LPARAM>(i));
        }

        if (browsers_.empty()) {
            SendMessageW(browserCombo_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(L"未检测到可配置的浏览器"));
            EnableWindow(browserCombo_, FALSE);
            SetWindowTextW(browserSummary_, L"没有找到历史结构可接入的浏览器。重新打开此页面可再次检测。");
            SetWindowTextW(pathLabel_, L"—");
        } else {
            detectedCount_ = detectedCount;
            UpdateBrowserSummary();
            const auto enabled = std::find_if(browsers_.begin(), browsers_.end(),
                [](const BrowserDefinition& browser) { return browser.enabled; });
            const LRESULT initialIndex = enabled == browsers_.end() ? 0 :
                static_cast<LRESULT>(std::distance(browsers_.begin(), enabled));
            SendMessageW(browserCombo_, CB_SETCURSEL, initialIndex, 0);
            SelectBrowser();
        }
        Button_SetCheck(bluetoothEnabledCheck_, bluetooth_.enabled ? BST_CHECKED : BST_UNCHECKED);
        SetWindowTextW(bluetoothKeywordEdit_, bluetooth_.keyword.c_str());
        UpdateBluetoothControls();
        SetFocus(browsers_.empty() ? bluetoothEnabledCheck_ : browserCombo_);
        return true;
    }

    LRESULT OnStaticColor(HDC deviceContext, HWND control) const {
        SetBkMode(deviceContext, TRANSPARENT);
        if (std::find(mutedControls_.begin(), mutedControls_.end(), control) != mutedControls_.end()) {
            SetTextColor(deviceContext, RGB(96, 96, 96));
        } else {
            SetTextColor(deviceContext, GetSysColor(COLOR_WINDOWTEXT));
        }
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    void OnDpiChanged(UINT dpi, const RECT* suggestedRect) {
        dpi_ = dpi;
        if (suggestedRect) {
            SetWindowPos(window_, nullptr, suggestedRect->left, suggestedRect->top,
                suggestedRect->right - suggestedRect->left,
                suggestedRect->bottom - suggestedRect->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        CreateFonts();
        LayoutControls();
        InvalidateRect(window_, nullptr, TRUE);
    }

    void SelectBrowser() {
        const LRESULT selected = SendMessageW(browserCombo_, CB_GETCURSEL, 0, 0);
        if (selected == CB_ERR) return;
        const auto index = static_cast<std::size_t>(selected);
        if (index >= browsers_.size()) return;
        selectedIndex_ = index;
        const auto& browser = browsers_[index];
        Button_SetCheck(enabledCheck_, browser.enabled ? BST_CHECKED : BST_UNCHECKED);
        SetWindowTextW(keywordEdit_, browser.prefix.c_str());
        std::filesystem::path icon = browser.iconPath;
        std::error_code iconError;
        if (ToLowerInvariant(icon.extension().wstring()) != L".ico" ||
            !std::filesystem::is_regular_file(icon, iconError)) {
            const auto cachedIcon = BrowserIcon::CachedIcoPath(browser.id);
            iconError.clear();
            if (std::filesystem::is_regular_file(cachedIcon, iconError)) {
                icon = cachedIcon;
            } else {
                std::wstring exportError;
                if (BrowserIcon::ExportIco(executables_[index], cachedIcon, exportError)) icon = cachedIcon;
                else icon.clear();
            }
        }
        SetWindowTextW(iconEdit_, icon.c_str());
        SetWindowTextW(pathLabel_, executables_[index].empty() ?
            L"未找到浏览器程序，当前只能停用此项" : executables_[index].c_str());
    }

    void SaveCurrentDraft() {
        if (selectedIndex_ >= browsers_.size()) return;
        auto& browser = browsers_[selectedIndex_];
        browser.enabled = Button_GetCheck(enabledCheck_) == BST_CHECKED;
        browser.prefix = ToLowerInvariant(Trim(ControlText(keywordEdit_)));
        browser.iconPath = Trim(ControlText(iconEdit_));
    }

    void UpdateBluetoothControls() const {
        const BOOL enabled = Button_GetCheck(bluetoothEnabledCheck_) == BST_CHECKED;
        EnableWindow(bluetoothKeywordEdit_, enabled);
    }

    bool SaveBluetoothDraft() {
        bluetooth_.enabled = Button_GetCheck(bluetoothEnabledCheck_) == BST_CHECKED;
        bluetooth_.keyword = ToLowerInvariant(Trim(ControlText(bluetoothKeywordEdit_)));
        if (bluetooth_.enabled && !ValidKeyword(bluetooth_.keyword)) {
            MessageBoxW(window_, L"蓝牙搜索关键字应为 1–16 个字母、数字、- 或 _。",
                L"配置 Listary", MB_OK | MB_ICONWARNING);
            SetFocus(bluetoothKeywordEdit_);
            return false;
        }
        return true;
    }

    void UpdateBrowserSummary() const {
        const auto enabledCount = std::count_if(browsers_.begin(), browsers_.end(),
            [](const BrowserDefinition& browser) { return browser.enabled; });
        const std::wstring summary = L"已检测到 " + std::to_wstring(detectedCount_) +
            L" 个可配置浏览器；本次将启用 " + std::to_wstring(enabledCount) +
            L" 个。重新打开此页面可刷新检测结果。";
        SetWindowTextW(browserSummary_, summary.c_str());
    }

    void BrowseIcon() {
        std::wstring buffer(32768, L'\0');
        const std::wstring current = ControlText(iconEdit_);
        std::copy_n(current.c_str(), std::min(current.size(), buffer.size() - 1), buffer.data());
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"ICO 图标文件 (*.ico)\0*.ico\0\0";
        dialog.lpstrFile = buffer.data();
        dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        if (GetOpenFileNameW(&dialog)) SetWindowTextW(iconEdit_, buffer.c_str());
    }

    void Apply() {
        SaveCurrentDraft();
        if (!SaveBluetoothDraft()) return;
        for (std::size_t index = 0; index < browsers_.size(); ++index) {
            const auto& browser = browsers_[index];
            if (!ValidKeyword(browser.prefix)) {
                MessageBoxW(window_, (browser.name +
                    L" 的搜索关键字应为 1–16 个字母、数字、- 或 _。").c_str(),
                    L"配置 Listary", MB_OK | MB_ICONWARNING);
                SendMessageW(browserCombo_, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
                SelectBrowser();
                SetFocus(keywordEdit_);
                return;
            }
            if (!browser.enabled) continue;
            if (executables_[index].empty()) {
                MessageBoxW(window_, (L"没有检测到 " + browser.name + L" 的可执行文件，不能启用。").c_str(),
                    L"配置 Listary", MB_OK | MB_ICONWARNING);
                SendMessageW(browserCombo_, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
                SelectBrowser();
                return;
            }
            std::error_code error;
            if (ToLowerInvariant(browser.iconPath.extension().wstring()) != L".ico" ||
                !std::filesystem::is_regular_file(browser.iconPath, error)) {
                MessageBoxW(window_, (browser.name + L" 的结果图标必须是存在的 .ico 文件。").c_str(),
                    L"配置 Listary", MB_OK | MB_ICONWARNING);
                SendMessageW(browserCombo_, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
                SelectBrowser();
                SetFocus(iconEdit_);
                return;
            }
        }
        result_ = ListarySettingsResult{browsers_, bluetooth_};
        DestroyWindow(window_);
    }

    LRESULT OnCommand(int id, int notification) {
        if (id == kBrowserId && notification == CBN_SELCHANGE) {
            SaveCurrentDraft();
            SelectBrowser();
        }
        else if (id == kEnabledId && notification == BN_CLICKED) {
            SaveCurrentDraft();
            UpdateBrowserSummary();
        }
        else if (id == kBrowseId && notification == BN_CLICKED) BrowseIcon();
        else if (id == kBluetoothEnabledId && notification == BN_CLICKED) UpdateBluetoothControls();
        else if (id == kApplyId && notification == BN_CLICKED) Apply();
        else if (id == IDCANCEL && notification == BN_CLICKED) DestroyWindow(window_);
        return 0;
    }

    HINSTANCE instance_{};
    HWND owner_{};
    HWND window_{};
    UINT dpi_ = 96;
    HFONT titleFont_{};
    HFONT bodyFont_{};
    HFONT sectionFont_{};
    HFONT smallFont_{};

    HWND titleLabel_{};
    HWND subtitleLabel_{};
    HWND topSeparator_{};
    HWND browserSection_{};
    HWND browserLabel_{};
    HWND browserCombo_{};
    HWND browserSummary_{};
    HWND pathTitle_{};
    HWND pathLabel_{};
    HWND middleSeparator_{};
    HWND listarySection_{};
    HWND enabledCheck_{};
    HWND keywordLabel_{};
    HWND keywordEdit_{};
    HWND keywordHint_{};
    HWND iconLabel_{};
    HWND iconEdit_{};
    HWND browseButton_{};
    HWND updateHint_{};
    HWND bluetoothSeparator_{};
    HWND bluetoothSection_{};
    HWND bluetoothEnabledCheck_{};
    HWND bluetoothKeywordLabel_{};
    HWND bluetoothKeywordEdit_{};
    HWND bluetoothKeywordHint_{};
    HWND bottomSeparator_{};
    HWND applyButton_{};
    HWND cancelButton_{};

    std::vector<HWND> controls_;
    std::vector<HWND> mutedControls_;
    std::vector<BrowserDefinition> browsers_;
    std::vector<std::filesystem::path> executables_;
    BluetoothConfig bluetooth_;
    std::size_t selectedIndex_ = static_cast<std::size_t>(-1);
    std::size_t detectedCount_ = 0;
    std::optional<ListarySettingsResult> result_;
};
}

std::optional<ListarySettingsResult> ListarySettingsDialog::Show(HINSTANCE instance, HWND owner,
    const AppConfig& config, std::wstring_view discoverySummary) {
    DialogState dialog(instance, owner, config, discoverySummary);
    return dialog.Run();
}
