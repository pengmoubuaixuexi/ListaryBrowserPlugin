#include "app_termination_tests.h"

#include "app_termination.h"
#include "browser_launcher.h"
#include "listary_suggestion_server.h"
#include "text_util.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr wchar_t kHelperArgument[] = L"--app-termination-test-helper";
constexpr wchar_t kWindowClass[] = L"BhlAppTerminationIsolatedTestWindow";

LRESULT CALLBACK TestWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_TIMER || message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        if (GetWindowLongPtrW(window, GWLP_USERDATA) == 1) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateTestWindow(std::wstring_view title) {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = TestWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    // A normal visible top-level window is needed for the real eligibility
    // checks. Keep it offscreen and do not activate it over the user's work.
    const std::wstring windowTitle(title);
    HWND window = CreateWindowExW(0, kWindowClass, windowTitle.c_str(), WS_OVERLAPPEDWINDOW,
        -30000, -30000, 280, 120, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (window) ShowWindow(window, SW_SHOWNOACTIVATE);
    return window;
}

class ScopedWindow {
public:
    explicit ScopedWindow(HWND value) : value_(value) {}
    ~ScopedWindow() { if (value_ && IsWindow(value_)) DestroyWindow(value_); }
    ScopedWindow(const ScopedWindow&) = delete;
    ScopedWindow& operator=(const ScopedWindow&) = delete;
    HWND get() const { return value_; }
private:
    HWND value_ = nullptr;
};

class ScopedHelper {
public:
    ~ScopedHelper() {
        if (!process_) return;
        if (Alive()) {
            DWORD owner = 0;
            if (window_) GetWindowThreadProcessId(window_, &owner);
            if (owner == processId_) PostMessageW(window_, WM_CLOSE, 0, 0);
            if (WaitForSingleObject(process_, 500) == WAIT_TIMEOUT) {
                // This is exclusively the handle returned for our own helper.
                TerminateProcess(process_, 2);
                WaitForSingleObject(process_, 3000);
            }
        }
        CloseHandle(process_);
    }
    ScopedHelper() = default;
    ScopedHelper(const ScopedHelper&) = delete;
    ScopedHelper& operator=(const ScopedHelper&) = delete;

    bool Start(const std::filesystem::path& executable, std::wstring_view title) {
        std::wstring command = BrowserLauncher::QuoteArgument(executable.wstring()) + L" " +
            kHelperArgument + L" " + BrowserLauncher::QuoteArgument(std::wstring(title));
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &information)) return false;
        CloseHandle(information.hThread);
        process_ = information.hProcess;
        processId_ = information.dwProcessId;
        const std::wstring expectedTitle(title);
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (Alive() && GetTickCount64() < deadline) {
            HWND candidate = FindWindowW(kWindowClass, expectedTitle.c_str());
            DWORD owner = 0;
            if (candidate) GetWindowThreadProcessId(candidate, &owner);
            if (owner == processId_ && IsWindowVisible(candidate)) {
                window_ = candidate;
                return true;
            }
            Sleep(20);
        }
        return false;
    }

    bool Alive() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }
    DWORD processId() const { return processId_; }
    HANDLE process() const { return process_; }

private:
    HANDLE process_ = nullptr;
    DWORD processId_ = 0;
    HWND window_ = nullptr;
};

class ScopedHelperCopy {
public:
    ~ScopedHelperCopy() {
        // Remove only the single file and directory created by this test.
        std::error_code ignored;
        if (!executable.empty()) std::filesystem::remove(executable, ignored);
        if (!directory.empty()) std::filesystem::remove(directory, ignored);
    }
    bool Create(const std::filesystem::path& root, std::wstring_view uniquePart) {
        std::array<wchar_t, 32768> currentExecutable{};
        const DWORD length = GetModuleFileNameW(nullptr, currentExecutable.data(),
            static_cast<DWORD>(currentExecutable.size()));
        if (length == 0 || length >= currentExecutable.size()) return false;
        const auto candidate = root / (L"app-termination-" + std::wstring(uniquePart));
        std::error_code error;
        if (!std::filesystem::create_directory(candidate, error)) return false;
        directory = candidate;
        // A different path/name is intentional: the production backend excludes
        // its own executable, BrowserHistoryLauncher, and every Listary process.
        executable = directory / L"IsolatedEndTestApp.exe";
        return std::filesystem::copy_file(currentExecutable.data(), executable,
            std::filesystem::copy_options::none, error);
    }

    std::filesystem::path directory;
    std::filesystem::path executable;
};

std::wstring SelectionUri(std::wstring_view selection) {
    std::wstring encoded;
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    for (const unsigned char byte : WideToUtf8(selection)) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            encoded.push_back(static_cast<wchar_t>(byte));
        } else {
            encoded.push_back(L'%');
            encoded.push_back(hex[byte >> 4]);
            encoded.push_back(hex[byte & 15]);
        }
    }
    return L"bhl://end?prefix=EnD&selection=" + encoded;
}

bool HasUnpairedSurrogate(std::wstring_view value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] >= 0xD800 && value[index] <= 0xDBFF) {
            if (++index >= value.size() || value[index] < 0xDC00 || value[index] > 0xDFFF) return true;
        } else if (value[index] >= 0xDC00 && value[index] <= 0xDFFF) {
            return true;
        }
    }
    return false;
}

void RunMappingTests(void (*check)(bool, const char*)) {
    AppConfig config;
    config.maxResults = 2;
    ListarySuggestionServer server(config);
    AppProcessTarget first{2147483001, 101, L"C:\\test-only\\Example.exe", L"示例应用", L"工作 & 中文"};
    AppProcessTarget second{2147483002, 102, first.executablePath, first.displayName, L"第二个窗口"};
    std::wstring error;
    const auto displays = server.CacheAppMappingsForTest(L"end", L"示例", {first, second});
    check(displays.size() == 2 && displays.front().find(L"强制结束") != std::wstring::npos &&
        displays.front().find(std::to_wstring(first.processId)) != std::wstring::npos,
        "end suggestions identify force termination and exact PID");
    check(!server.ResolveUri(SelectionUri(L"示例"), error), "end raw query never selects the first application");
    if (displays.empty()) return;
    const auto selected = server.ResolveUri(SelectionUri(displays.front()), error);
    check(selected && selected->kind == ListaryActionKind::AppTerminate &&
        selected->appTarget.processId == first.processId && selected->appTarget.creationTime == first.creationTime &&
        selected->appTarget.executablePath == first.executablePath && selected->appTarget.displayName == first.displayName &&
        selected->appTarget.windowTitle == first.windowTitle,
        "end selection preserves complete process identity and Unicode label");

    const auto refreshed = server.CacheAppMappingsForTest(L"end", L"第二", {second});
    check(refreshed.size() == 1 && !server.ResolveUri(SelectionUri(displays.front()), error) &&
        server.ResolveUri(SelectionUri(refreshed.front()), error).has_value(),
        "end refresh removes old process mappings");
    server.CacheAppMappingsForTest(L"end", L"no matches", {});
    check(refreshed.empty() || !server.ResolveUri(SelectionUri(refreshed.front()), error),
        "empty end refresh removes previously selectable applications");

    const auto expired = server.CacheAppMappingsForTest(L"end", L"示例", {first}, std::chrono::milliseconds(-1));
    check(expired.size() == 1 && !server.ResolveUri(SelectionUri(expired.front()), error) &&
        error.find(L"过期") != std::wstring::npos && error.find(L"蓝牙") == std::wstring::npos,
        "end mappings expire without a Bluetooth-specific error");
    const auto rawCollision = server.CacheAppMappingsForTest(L"end", displays.front(), {first});
    check(rawCollision.empty() && !server.ResolveUri(SelectionUri(displays.front()), error),
        "end raw input identical to a display row cannot terminate an application");

    auto emojiBoundary = first;
    emojiBoundary.displayName = std::wstring(79, L'b') + L"\U0001F600tail";
    emojiBoundary.windowTitle = std::wstring(99, L'a') + L"\U0001F600tail";
    const auto emojiDisplays = server.CacheAppMappingsForTest(L"end", L"boundary", {emojiBoundary});
    check(emojiDisplays.size() == 1 && !HasUnpairedSurrogate(emojiDisplays.front()) &&
        !WideToUtf8(emojiDisplays.front()).empty() &&
        server.ResolveUri(SelectionUri(emojiDisplays.front()), error).has_value(),
        "end display truncation preserves UTF-16 emoji boundaries and selectable URI");

    config.maxResults = 1;
    ListarySuggestionServer limited(config);
    check(limited.CacheAppMappingsForTest(L"end", L"示例", {first, second}).size() == 1,
        "end mappings honor configured result limit");
    config.maxResults = 0;
    ListarySuggestionServer empty(config);
    check(empty.CacheAppMappingsForTest(L"end", L"示例", {first}).empty(),
        "end zero result limit produces no mappings");
    auto incomplete = first;
    incomplete.creationTime = 0;
    check(server.CacheAppMappingsForTest(L"end", L"示例", {incomplete}).empty(),
        "end mappings reject incomplete process identity");
}
}

bool TryRunAppTerminationTestHelper(int argc, wchar_t** argv, int& exitCode) {
    if (argc < 2 || std::wstring_view(argv[1]) != kHelperArgument) return false;
    exitCode = 4;
    if (argc != 3) return true;
    HWND window = CreateTestWindow(argv[2]);
    if (!window) return true;
    SetWindowLongPtrW(window, GWLP_USERDATA, 1);
    // Also self-expire if the parent crashes before its RAII cleanup can run.
    if (!SetTimer(window, 1, 60000, nullptr)) {
        DestroyWindow(window);
        return true;
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    exitCode = 0;
    return true;
}

void RunAppTerminationTests(void (*check)(bool, const char*), const std::filesystem::path& tempRoot) {
    RunMappingTests(check);
    const auto uniquePart = std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(std::random_device{}());
    const auto commonTitle = L"BHL End Test " + uniquePart;
    const auto firstTitle = commonTitle + L" 中文甲";
    const auto secondTitle = commonTitle + L" 中文乙";
    ScopedHelperCopy copy;
    const bool copied = copy.Create(tempRoot, uniquePart);
    check(copied, "create isolated copy for application termination helpers");
    if (!copied) return;
    ScopedHelper first;
    ScopedHelper second;
    const bool firstReady = first.Start(copy.executable, firstTitle);
    const bool secondReady = second.Start(copy.executable, secondTitle);
    check(firstReady && secondReady, "start two independent visible helper applications");
    if (!firstReady || !secondReady) return;

    const ScopedWindow ownWindow(CreateTestWindow(commonTitle + L" parent"));
    check(ownWindow.get() != nullptr, "create current-process window for self-exclusion test");
    const auto targets = AppTermination::Enumerate(commonTitle, 10);
    const auto firstTarget = std::find_if(targets.begin(), targets.end(), [&](const auto& target) {
        return target.processId == first.processId();
    });
    const auto secondTarget = std::find_if(targets.begin(), targets.end(), [&](const auto& target) {
        return target.processId == second.processId();
    });
    check(targets.size() == 2 && firstTarget != targets.end() && secondTarget != targets.end(),
        "application enumeration finds exact helper PIDs by window title");
    check(std::none_of(targets.begin(), targets.end(), [](const auto& target) {
        return target.processId == GetCurrentProcessId();
    }), "application enumeration excludes the caller even with a matching visible window");
    check(AppTermination::Enumerate(commonTitle, 1).size() == 1 &&
        AppTermination::Enumerate(commonTitle, 0).empty(),
        "application enumeration honors result limit and zero limit");
    const auto chineseMatch = AppTermination::Enumerate(uniquePart + L" 中文甲", 10);
    check(chineseMatch.size() == 1 && chineseMatch.front().processId == first.processId() &&
        chineseMatch.front().windowTitle == firstTitle,
        "application enumeration matches Unicode window titles");
    if (firstTarget == targets.end() || secondTarget == targets.end()) return;

    auto wrongCreation = *firstTarget;
    ++wrongCreation.creationTime;
    check(!AppTermination::Terminate(wrongCreation).succeeded && first.Alive() && second.Alive(),
        "forged process creation time is rejected without ending either helper");
    auto wrongPath = *firstTarget;
    wrongPath.executablePath += L".not-the-target";
    check(!AppTermination::Terminate(wrongPath).succeeded && first.Alive() && second.Alive(),
        "forged executable path is rejected without ending either helper");
    auto wrongPid = *firstTarget;
    wrongPid.processId = second.processId();
    // The second helper may share the same clock tick, so make the identity
    // mismatch explicit while addressing only another process we created.
    wrongPid.creationTime = secondTarget->creationTime + 1;
    check(!AppTermination::Terminate(wrongPid).succeeded && first.Alive() && second.Alive(),
        "mismatched PID identity cannot redirect termination to the other helper");
    const auto ended = AppTermination::Terminate(*firstTarget);
    check(ended.succeeded && WaitForSingleObject(first.process(), 0) == WAIT_OBJECT_0,
        "real termination confirms the selected helper has exited");
    check(second.Alive(), "ending one helper preserves another instance of the same executable");
    check(!AppTermination::Terminate(*firstTarget).succeeded && second.Alive(),
        "already exited target is rejected without touching the other helper");
}
