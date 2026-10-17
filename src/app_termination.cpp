#include "app_termination.h"

#include "text_util.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <set>
#include <utility>

#pragma comment(lib, "dwmapi.lib")

namespace {
constexpr DWORD kTerminationWaitMs = 1500;
constexpr std::size_t kMaximumResults = 100;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~ScopedHandle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};

struct ProcessOwner {
    std::vector<BYTE> userSid;
    DWORD sessionId = 0;
};

std::wstring FileName(std::wstring_view path) {
    const auto separator = path.find_last_of(L"\\/");
    return std::wstring(separator == std::wstring_view::npos ? path : path.substr(separator + 1));
}

bool SameText(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() && CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool ReadOwner(HANDLE process, ProcessOwner& owner) {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &rawToken)) return false;
    const ScopedHandle token(rawToken);
    DWORD bytes = 0;
    if (GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 || bytes > 64 * 1024) return false;
    std::vector<BYTE> buffer(bytes);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), bytes, &bytes)) return false;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    if (!IsValidSid(user->User.Sid)) return false;
    owner.userSid.resize(GetLengthSid(user->User.Sid));
    if (!CopySid(static_cast<DWORD>(owner.userSid.size()), owner.userSid.data(), user->User.Sid)) return false;
    return GetTokenInformation(token.get(), TokenSessionId, &owner.sessionId,
        sizeof(owner.sessionId), &bytes) != FALSE;
}

bool ReadProcessIdentity(HANDLE process, std::uint64_t& creationTime, std::wstring& executablePath) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
    creationTime = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    std::array<wchar_t, 32768> path{};
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size) || size == 0) return false;
    executablePath.assign(path.data(), size);
    return creationTime != 0;
}

bool ExcludedExecutable(std::wstring_view executablePath, std::wstring_view ownExecutable) {
    if (SameText(executablePath, ownExecutable)) return true;
    const auto name = ToLowerInvariant(FileName(executablePath));
    if (name == L"browserhistorylauncher.exe" || name.starts_with(L"listary")) return true;
    // Shell and infrastructure processes are not ordinary app targets, including
    // those that happen to own a visible window or are not flagged as critical.
    constexpr std::wstring_view excluded[] = {
        L"system", L"registry", L"secure system", L"smss.exe", L"csrss.exe",
        L"wininit.exe", L"winlogon.exe", L"services.exe", L"lsass.exe", L"lsaiso.exe",
        L"svchost.exe", L"fontdrvhost.exe", L"dwm.exe", L"sihost.exe", L"taskhost.exe",
        L"taskhostw.exe", L"runtimebroker.exe", L"explorer.exe", L"shellexperiencehost.exe",
        L"startmenuexperiencehost.exe", L"searchhost.exe", L"searchapp.exe", L"searchui.exe",
        L"searchindexer.exe", L"applicationframehost.exe", L"audiodg.exe", L"spoolsv.exe",
        L"conhost.exe", L"openconsole.exe", L"ctfmon.exe", L"textinputhost.exe",
        L"securityhealthservice.exe", L"msmpeng.exe", L"win32webviewhost.exe",
        L"credentialuibroker.exe", L"logonui.exe", L"consent.exe", L"dllhost.exe"
    };
    return std::find(std::begin(excluded), std::end(excluded), name) != std::end(excluded);
}

bool AllowedProcess(HANDLE process, DWORD processId, const ProcessOwner& ownOwner,
    std::wstring_view ownExecutable, AppProcessTarget& identity) {
    if (processId == 0 || processId == 4 || processId == GetCurrentProcessId() ||
        GetProcessId(process) != processId) return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process, &exitCode) || exitCode != STILL_ACTIVE) return false;
    BOOL critical = TRUE;
    if (!IsProcessCritical(process, &critical) || critical) return false;
    PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
    if (!GetProcessInformation(process, ProcessProtectionLevelInfo, &protection, sizeof(protection)) ||
        protection.ProtectionLevel != PROTECTION_LEVEL_NONE) return false;
    ProcessOwner owner;
    if (!ReadOwner(process, owner) || owner.sessionId == 0 || owner.sessionId != ownOwner.sessionId ||
        owner.userSid != ownOwner.userSid) return false;
    if (!ReadProcessIdentity(process, identity.creationTime, identity.executablePath) ||
        ExcludedExecutable(identity.executablePath, ownExecutable)) return false;
    identity.processId = processId;
    identity.displayName = FileName(identity.executablePath);
    if (identity.displayName.size() > 4 &&
        SameText(std::wstring_view(identity.displayName).substr(identity.displayName.size() - 4), L".exe")) {
        identity.displayName.resize(identity.displayName.size() - 4);
    }
    return true;
}

bool OrdinaryWindow(HWND window) {
    if (!IsWindowVisible(window) || GetAncestor(window, GA_ROOT) != window || GetWindow(window, GW_OWNER)) return false;
    const auto extendedStyle = static_cast<DWORD_PTR>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    if ((extendedStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0) return false;
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked != 0) return false;
    return true;
}

struct EnumerationContext {
    ProcessOwner owner;
    std::wstring ownExecutable;
    std::wstring query;
    std::size_t limit = 0;
    std::set<DWORD> included;
    std::vector<AppProcessTarget> results;
    bool failed = false;
};

BOOL CALLBACK EnumerateWindow(HWND window, LPARAM parameter) noexcept {
    auto& context = *reinterpret_cast<EnumerationContext*>(parameter);
    try {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId == 0 || processId == GetCurrentProcessId() || context.included.contains(processId) ||
            !OrdinaryWindow(window)) return TRUE;
        std::array<wchar_t, 512> title{};
        const int titleLength = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
        if (titleLength <= 0) return TRUE;
        const ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
        if (!process) return TRUE;
        AppProcessTarget target;
        if (!AllowedProcess(process.get(), processId, context.owner, context.ownExecutable, target)) return TRUE;
        target.windowTitle.assign(title.data(), static_cast<std::size_t>(titleLength));
        if (!context.query.empty() &&
            ToLowerInvariant(target.windowTitle).find(context.query) == std::wstring::npos &&
            ToLowerInvariant(FileName(target.executablePath)).find(context.query) == std::wstring::npos) return TRUE;
        context.included.insert(processId);
        context.results.push_back(std::move(target));
        return context.results.size() < context.limit ? TRUE : FALSE;
    } catch (...) {
        context.failed = true;
        return FALSE;
    }
}

struct WindowCheck {
    DWORD processId = 0;
    bool found = false;
};

BOOL CALLBACK FindApplicationWindow(HWND window, LPARAM parameter) noexcept {
    auto& check = *reinterpret_cast<WindowCheck*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != check.processId || !OrdinaryWindow(window)) return TRUE;
    std::array<wchar_t, 2> title{};
    if (GetWindowTextW(window, title.data(), static_cast<int>(title.size())) <= 0) return TRUE;
    check.found = true;
    return FALSE;
}

bool ReadCaller(ProcessOwner& owner, std::wstring& executable) {
    std::uint64_t created = 0;
    return ReadOwner(GetCurrentProcess(), owner) && owner.sessionId != 0 &&
        ReadProcessIdentity(GetCurrentProcess(), created, executable);
}
}

std::vector<AppProcessTarget> AppTermination::Enumerate(std::wstring_view query, std::size_t maxResults) {
    if (maxResults == 0) return {};
    EnumerationContext context;
    if (!ReadCaller(context.owner, context.ownExecutable)) return {};
    context.query = ToLowerInvariant(Trim(query));
    context.limit = std::min(maxResults, kMaximumResults);
    EnumWindows(EnumerateWindow, reinterpret_cast<LPARAM>(&context));
    if (context.failed) return {};
    return std::move(context.results);
}

AppTerminationResult AppTermination::Terminate(const AppProcessTarget& target) {
    if (target.processId == 0 || target.creationTime == 0 || target.executablePath.empty()) {
        return {false, L"应用目标信息不完整，请重新搜索。"};
    }
    ProcessOwner owner;
    std::wstring ownExecutable;
    if (!ReadCaller(owner, ownExecutable)) return {false, L"无法验证当前用户，未结束任何应用。"};

    // Keep this handle through validation, TerminateProcess and the bounded wait.
    // A stale PID can never redirect this operation to a replacement process.
    const ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE, target.processId));
    if (!process) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return {false, L"该应用已经退出，请重新搜索。"};
        return {false, L"无法获得结束此应用的权限，请在应用内退出。"};
    }
    AppProcessTarget current;
    if (!AllowedProcess(process.get(), target.processId, owner, ownExecutable, current) ||
        current.creationTime != target.creationTime || !SameText(current.executablePath, target.executablePath)) {
        return {false, L"应用已退出、发生变化或不允许结束，请重新搜索。"};
    }
    WindowCheck windowCheck{target.processId};
    EnumWindows(FindApplicationWindow, reinterpret_cast<LPARAM>(&windowCheck));
    if (!windowCheck.found) return {false, L"该应用已没有可见窗口，请重新搜索。"};
    // The protected/critical state may have changed while windows were checked.
    BOOL critical = TRUE;
    PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
    if (!IsProcessCritical(process.get(), &critical) || critical ||
        !GetProcessInformation(process.get(), ProcessProtectionLevelInfo, &protection, sizeof(protection)) ||
        protection.ProtectionLevel != PROTECTION_LEVEL_NONE) {
        return {false, L"该应用受系统保护，未执行结束操作。"};
    }
    if (!TerminateProcess(process.get(), 1)) return {false, L"Windows 未能结束该应用，请重新搜索确认状态。"};
    const DWORD wait = WaitForSingleObject(process.get(), kTerminationWaitMs);
    if (wait != WAIT_OBJECT_0) {
        return {false, L"已发送强制结束请求，但尚未确认退出，请稍后重新搜索。"};
    }
    return {true, L"已结束“" + current.displayName + L"”（PID " + std::to_wstring(current.processId) + L"）。"};
}
