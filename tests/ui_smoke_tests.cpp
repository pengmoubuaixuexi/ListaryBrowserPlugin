#include <Windows.h>
#include <CommCtrl.h>
#include <Psapi.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"BrowserHistoryLauncher.SearchWindow";
constexpr UINT kShowMessage = WM_APP + 11;
constexpr UINT kExitMessage = WM_APP + 13;
int failures = 0;

void Check(bool condition, const char* name) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    if (!condition) ++failures;
}

HWND WaitForWindow(DWORD timeoutMs) {
    const auto deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        if (HWND window = FindWindowW(kWindowClass, nullptr)) return window;
        Sleep(20);
    }
    return nullptr;
}

std::wstring WindowText(HWND window) {
    const int length = static_cast<int>(SendMessageW(window, WM_GETTEXTLENGTH, 0, 0));
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(window, WM_GETTEXT, static_cast<WPARAM>(length + 1),
        reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<std::size_t>(length));
    return text;
}

PROCESS_MEMORY_COUNTERS_EX2 Memory(HANDLE process) {
    PROCESS_MEMORY_COUNTERS_EX2 memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    return memory;
}

SIZE_T PrivateWorkingSet(HANDLE process) {
    std::vector<unsigned char> buffer(1024 * 1024);
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto* information = reinterpret_cast<PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
        if (QueryWorkingSet(process, information, static_cast<DWORD>(buffer.size()))) {
            SYSTEM_INFO system{};
            GetSystemInfo(&system);
            SIZE_T privatePages = 0;
            for (ULONG_PTR i = 0; i < information->NumberOfEntries; ++i) {
                if (!information->WorkingSetInfo[i].Shared) ++privatePages;
            }
            return privatePages * system.dwPageSize;
        }
        if (GetLastError() != ERROR_BAD_LENGTH) break;
        buffer.resize(buffer.size() * 2);
    }
    return 0;
}

double CpuPercent(HANDLE process, DWORD sampleMs) {
    FILETIME creation{}, exit{}, kernel1{}, user1{}, kernel2{}, user2{};
    GetProcessTimes(process, &creation, &exit, &kernel1, &user1);
    Sleep(sampleMs);
    GetProcessTimes(process, &creation, &exit, &kernel2, &user2);
    ULARGE_INTEGER k1{}, u1{}, k2{}, u2{};
    k1.LowPart = kernel1.dwLowDateTime; k1.HighPart = kernel1.dwHighDateTime;
    u1.LowPart = user1.dwLowDateTime; u1.HighPart = user1.dwHighDateTime;
    k2.LowPart = kernel2.dwLowDateTime; k2.HighPart = kernel2.dwHighDateTime;
    u2.LowPart = user2.dwLowDateTime; u2.HighPart = user2.dwHighDateTime;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const double used100ns = static_cast<double>((k2.QuadPart - k1.QuadPart) + (u2.QuadPart - u1.QuadPart));
    return used100ns / (static_cast<double>(sampleMs) * 10000.0 * info.dwNumberOfProcessors) * 100.0;
}

bool LaunchQuery(const wchar_t* application, std::wstring_view query) {
    std::wstring command = L"\"" + std::wstring(application) + L"\" --query \"" + std::wstring(query) + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(application, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0;
}
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2) {
        std::cerr << "usage: UiSmokeTests <BrowserHistoryLauncher.exe>\n";
        return 2;
    }
    Check(FindWindowW(kWindowClass, nullptr) == nullptr, "no pre-existing launcher instance");
    if (failures) return 1;

    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(argv[1], command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        Check(false, "start Release executable");
        return 1;
    }
    CloseHandle(process.hThread);
    HWND window = WaitForWindow(5000);
    Check(window != nullptr, "Release window created");
    if (!window) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        return 1;
    }
    Sleep(800);
    Check(!IsWindowVisible(window), "Listary-only startup keeps the native window hidden");
    HWND edit = FindWindowExW(window, nullptr, L"Edit", nullptr);
    HWND list = FindWindowExW(window, nullptr, WC_LISTVIEWW, nullptr);
    Check(edit == nullptr && list == nullptr, "Listary-only startup defers native search controls");
    const auto listaryIdlePrivateWorkingSet = PrivateWorkingSet(process.hProcess);
    std::cout << "METRIC listary_idle_private_working_set_bytes=" << listaryIdlePrivateWorkingSet << '\n';
    Check(listaryIdlePrivateWorkingSet > 0 && listaryIdlePrivateWorkingSet <= 15ULL * 1024 * 1024,
        "Listary-only private working set <= 15 MB target");

    Check(LaunchQuery(argv[1], L"g"), "first --query invocation reaches the hidden instance");
    const auto visibleDeadline = GetTickCount64() + 5000;
    while (GetTickCount64() < visibleDeadline && !IsWindowVisible(window)) Sleep(10);
    Check(IsWindowVisible(window), "first --query invocation shows the native fallback window");
    edit = nullptr;
    list = nullptr;
    const auto controlsDeadline = GetTickCount64() + 5000;
    while (GetTickCount64() < controlsDeadline && (!edit || !list)) {
        edit = FindWindowExW(window, nullptr, L"Edit", nullptr);
        list = FindWindowExW(window, nullptr, WC_LISTVIEWW, nullptr);
        if (!edit || !list) Sleep(10);
    }
    Check(edit != nullptr && list != nullptr, "native Edit and ListView controls exist");
    HWND configButton = FindWindowExW(window, nullptr, L"Button", L"配置 Listary");
    Check(configButton != nullptr, "main window exposes the Listary configuration button");
    if (configButton) {
        PostMessageW(configButton, BM_CLICK, 0, 0);
        HWND settingsWindow = nullptr;
        const auto settingsDeadline = GetTickCount64() + 5000;
        while (GetTickCount64() < settingsDeadline && !settingsWindow) {
            settingsWindow = FindWindowW(L"BrowserHistoryLauncher.ListarySettings", nullptr);
            if (!settingsWindow) Sleep(10);
        }
        Check(settingsWindow != nullptr, "Listary configuration dialog opens from the main window");
        if (settingsWindow) {
            Check(GetClassLongPtrW(settingsWindow, GCLP_HICON) != 0,
                "configuration dialog uses the embedded application icon");
            RECT settingsClient{};
            GetClientRect(settingsWindow, &settingsClient);
            const UINT settingsDpi = GetDpiForWindow(settingsWindow);
            std::cout << "METRIC settings_dpi=" << settingsDpi
                      << " settings_client_width=" << settingsClient.right
                      << " settings_client_height=" << settingsClient.bottom << '\n';
            Check(settingsClient.right >= 678 && settingsClient.bottom >= 508,
                "configuration dialog provides its designed client area");
            HWND applyButton = nullptr;
            const auto applyDeadline = GetTickCount64() + 5000;
            while (GetTickCount64() < applyDeadline && !applyButton) {
                applyButton = FindWindowExW(settingsWindow, nullptr, L"Button", L"保存当前浏览器");
                if (!applyButton) Sleep(10);
            }
            Check(applyButton != nullptr,
                "configuration dialog exposes a clear primary action");
            const HWND iconEdit = GetDlgItem(settingsWindow, 1004);
            std::filesystem::path iconPath;
            const auto iconDeadline = GetTickCount64() + 5000;
            while (GetTickCount64() < iconDeadline && iconPath.empty()) {
                if (iconEdit) iconPath = WindowText(iconEdit);
                if (iconPath.empty()) Sleep(10);
            }
            std::error_code iconError;
            Check(!iconPath.empty() && _wcsicmp(iconPath.extension().c_str(), L".ico") == 0 &&
                  std::filesystem::is_regular_file(iconPath, iconError),
                "configuration dialog auto-generates a real ICO file");
            HWND browserCombo = nullptr;
            LRESULT browserCount = 0;
            const auto browserDeadline = GetTickCount64() + 5000;
            while (GetTickCount64() < browserDeadline && browserCount < 3) {
                browserCombo = FindWindowExW(settingsWindow, nullptr, WC_COMBOBOXW, nullptr);
                if (browserCombo) browserCount = SendMessageW(browserCombo, CB_GETCOUNT, 0, 0);
                if (browserCount < 3) Sleep(10);
            }
            Check(browserCombo != nullptr && browserCount >= 3,
                "browser dropdown contains system-discovered compatible browsers");
            Check(browserCombo && SendMessageW(browserCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                reinterpret_cast<LPARAM>(L"Quark")) != CB_ERR,
                "browser dropdown discovers registered Quark without a predefined INI section");
            RECT comboRect{};
            RECT applyRect{};
            if (browserCombo) GetWindowRect(browserCombo, &comboRect);
            if (applyButton) GetWindowRect(applyButton, &applyRect);
            POINT clientOrigin{};
            ClientToScreen(settingsWindow, &clientOrigin);
            const int clientRight = clientOrigin.x + settingsClient.right;
            const int clientBottom = clientOrigin.y + settingsClient.bottom;
            bool allControlsInside = true;
            for (HWND child = GetWindow(settingsWindow, GW_CHILD); child;
                 child = GetWindow(child, GW_HWNDNEXT)) {
                if (!IsWindowVisible(child)) continue;
                RECT childRect{};
                GetWindowRect(child, &childRect);
                if (childRect.left < clientOrigin.x || childRect.top < clientOrigin.y ||
                    childRect.right > clientRight || childRect.bottom > clientBottom) {
                    allControlsInside = false;
                    break;
                }
            }
            Check(browserCombo && applyButton && allControlsInside,
                "all configuration controls remain inside the client area");
            SendMessageW(settingsWindow, WM_CLOSE, 0, 0);
        }
    }
    Check(GetClassLongPtrW(window, GCLP_HICON) != 0, "embedded application icon is assigned to the window");
    const auto initialQueryDeadline = GetTickCount64() + 5000;
    while (GetTickCount64() < initialQueryDeadline && ListView_GetItemCount(list) == 0) Sleep(10);
    Check(ListView_GetItemCount(list) > 0, "first --query invocation populates Chrome history");

    SendMessageW(window, WM_CLOSE, 0, 0);
    Sleep(800);
    const auto idleMemory = Memory(process.hProcess);
    const auto idlePrivateWorkingSet = PrivateWorkingSet(process.hProcess);
    const double idleCpu = CpuPercent(process.hProcess, 1000);
    std::cout << "METRIC idle_private_working_set_bytes=" << idlePrivateWorkingSet
              << " idle_working_set_bytes=" << idleMemory.WorkingSetSize
              << " idle_private_commit_bytes=" << idleMemory.PrivateUsage
              << " idle_cpu_percent=" << idleCpu << '\n';
    Check(idlePrivateWorkingSet > 0 && idlePrivateWorkingSet <= 15ULL * 1024 * 1024, "idle private working set <= 15 MB target");

    const auto activationStart = std::chrono::steady_clock::now();
    PostMessageW(window, kShowMessage, 0, 0);
    while (!IsWindowVisible(window)) Sleep(1);
    const auto activationUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - activationStart).count();
    std::cout << "METRIC warm_activation_us=" << activationUs << '\n';
    Check(activationUs < 100000, "warm activation under 100 ms");
    edit = FindWindowExW(window, nullptr, L"Edit", nullptr);
    list = FindWindowExW(window, nullptr, WC_LISTVIEWW, nullptr);
    Check(edit != nullptr && list != nullptr, "search controls available after hide");

    const auto queryStart = std::chrono::steady_clock::now();
    Check(LaunchQuery(argv[1], L"g"), "--query command forwards Chrome query to existing instance");
    int itemCount = 0;
    const auto deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        itemCount = ListView_GetItemCount(list);
        if (itemCount > 0) break;
        Sleep(10);
    }
    const auto queryUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - queryStart).count();
    if (itemCount == 0) {
        HWND status = FindWindowExW(window, edit, L"Static", nullptr);
        if (!status) status = FindWindowExW(window, nullptr, L"Static", nullptr);
        std::wcout << L"DIAGNOSTIC ui_status=" << (status ? WindowText(status) : L"<not found>") << L'\n';
    }
    Check(itemCount > 0, "g query populates dropdown from Chrome history");
    const auto queryMemory = Memory(process.hProcess);
    const auto queryPrivateWorkingSet = PrivateWorkingSet(process.hProcess);
    std::cout << "METRIC chrome_dropdown_results=" << itemCount
              << " end_to_end_query_us=" << queryUs
              << " query_private_working_set_bytes=" << queryPrivateWorkingSet << '\n';
    Check(queryPrivateWorkingSet > 0 && queryPrivateWorkingSet <= 30ULL * 1024 * 1024, "query private working set <= 30 MB target");

    Check(LaunchQuery(argv[1], L"e"), "--query command forwards Edge query to existing instance");
    Sleep(1200);
    const int edgeItemCount = ListView_GetItemCount(list);
    std::cout << "METRIC edge_dropdown_results=" << edgeItemCount << '\n';
    Check(edgeItemCount > 0, "e query populates dropdown from Edge history");

    SendMessageW(window, WM_CLOSE, 0, 0);
    Sleep(300);
    const auto beforeCycles = PrivateWorkingSet(process.hProcess);
    for (int i = 0; i < 100; ++i) {
        SendMessageW(window, kShowMessage, 0, 0);
        SendMessageW(window, WM_CLOSE, 0, 0);
    }
    Sleep(500);
    const auto afterCycles = PrivateWorkingSet(process.hProcess);
    std::cout << "METRIC cycle_count=100 private_ws_before=" << beforeCycles
              << " private_ws_after=" << afterCycles << '\n';
    Check(afterCycles <= beforeCycles + 1024 * 1024, "100 show/hide cycles do not grow private working set by >1 MB");

    PostMessageW(window, kExitMessage, 0, 0);
    const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
    Check(wait == WAIT_OBJECT_0, "normal exit leaves no launcher process");
    CloseHandle(process.hProcess);
    std::cout << "SUMMARY failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
