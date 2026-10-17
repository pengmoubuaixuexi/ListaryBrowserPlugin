#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct AppProcessTarget {
    DWORD processId = 0;
    std::uint64_t creationTime = 0;
    std::wstring executablePath;
    std::wstring displayName;
    std::wstring windowTitle;
};

struct AppTerminationResult {
    bool succeeded = false;
    std::wstring message;
};

namespace AppTermination {
// Only ordinary, visible application windows owned by the current user/session.
// Enumeration never requests PROCESS_TERMINATE or changes another process.
std::vector<AppProcessTarget> Enumerate(std::wstring_view query, std::size_t maxResults);

// The cached identity is revalidated on the same handle used for termination.
// Only this process is ended; no executable-name or process-tree expansion occurs.
AppTerminationResult Terminate(const AppProcessTarget& target);
}
