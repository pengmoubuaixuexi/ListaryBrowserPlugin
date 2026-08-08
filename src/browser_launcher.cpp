#include "browser_launcher.h"

#include "text_util.h"

#include <Windows.h>

#include <cwctype>
#include <vector>

std::filesystem::path BrowserLauncher::FindExecutable(const BrowserDefinition& browser) {
    for (const auto& candidate : browser.executableCandidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

std::wstring BrowserLauncher::QuoteArgument(std::wstring_view argument) {
    if (argument.empty()) return L"\"\"";
    bool needsQuotes = false;
    for (const wchar_t ch : argument) {
        if (std::iswspace(ch) || ch == L'\"') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) return std::wstring(argument);

    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(ch);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool BrowserLauncher::OpenUrl(const BrowserDefinition& browser, std::wstring_view profileDirectory,
    std::wstring_view url, std::wstring& error) {
    const auto executable = FindExecutable(browser);
    if (executable.empty()) {
        error = browser.name + L" 不可用，未找到配置的可执行文件。";
        return false;
    }

    std::vector<std::wstring> arguments;
    if (!profileDirectory.empty() && !browser.profileArgument.empty()) {
        std::wstring profileArgument = browser.profileArgument;
        const std::wstring marker = L"{profile}";
        const auto position = profileArgument.find(marker);
        if (position != std::wstring::npos) {
            profileArgument.replace(position, marker.size(), profileDirectory);
        }
        arguments.push_back(std::move(profileArgument));
    }
    arguments.emplace_back(url);

    std::wstring commandLine = QuoteArgument(executable.wstring());
    for (const auto& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += QuoteArgument(argument);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0,
            nullptr, executable.parent_path().c_str(), &startup, &process)) {
        error = L"无法启动 " + browser.name + L"：" + FormatWindowsError(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
