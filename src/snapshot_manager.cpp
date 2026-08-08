#include "snapshot_manager.h"

#include "text_util.h"

#include <array>
#include <chrono>
#include <functional>
#include <sstream>

namespace {
bool ReadFileIdentity(const std::filesystem::path& path, std::uintmax_t& size, FILETIME& writeTime) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    size = (static_cast<std::uintmax_t>(data.nFileSizeHigh) << 32U) | data.nFileSizeLow;
    writeTime = data.ftLastWriteTime;
    return true;
}

bool SameFileTime(const FILETIME& left, const FILETIME& right) {
    return left.dwLowDateTime == right.dwLowDateTime && left.dwHighDateTime == right.dwHighDateTime;
}

void DeleteSnapshotFamily(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.wstring() + L"-wal", ignored);
    std::filesystem::remove(path.wstring() + L"-shm", ignored);
}
}

SnapshotManager::SnapshotManager() {
    wchar_t buffer[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    tempDirectory_ = length > 0 ? std::filesystem::path(buffer) / L"BrowserHistoryLauncher" :
                                 std::filesystem::temp_directory_path() / L"BrowserHistoryLauncher";
    std::error_code ignored;
    std::filesystem::create_directories(tempDirectory_, ignored);
    CleanupExpired();
}

SnapshotManager::~SnapshotManager() {
    Clear();
}

std::filesystem::path SnapshotManager::SnapshotPathFor(const std::filesystem::path& source) const {
    std::wostringstream name;
    name << L"bhl-" << GetCurrentProcessId() << L'-' << std::hex << std::hash<std::wstring>{}(source.wstring())
         << L".sqlite";
    return tempDirectory_ / name.str();
}

bool SnapshotManager::StreamCopy(const std::filesystem::path& source,
    const std::filesystem::path& destination, std::wstring& error) {
    HANDLE input = CreateFileW(source.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        error = L"无法读取历史数据库快照源：" + FormatWindowsError(GetLastError());
        return false;
    }
    HANDLE output = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        error = L"无法创建历史数据库快照：" + FormatWindowsError(GetLastError());
        CloseHandle(input);
        return false;
    }

    std::array<unsigned char, 64 * 1024> buffer{};
    bool ok = true;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            error = L"读取历史数据库快照失败：" + FormatWindowsError(GetLastError());
            ok = false;
            break;
        }
        if (read == 0) break;
        DWORD written = 0;
        if (!WriteFile(output, buffer.data(), read, &written, nullptr) || written != read) {
            error = L"写入历史数据库快照失败：" + FormatWindowsError(GetLastError());
            ok = false;
            break;
        }
    }
    CloseHandle(output);
    CloseHandle(input);
    if (!ok) {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
    }
    return ok;
}

std::filesystem::path SnapshotManager::CreateOrReuse(const std::filesystem::path& source,
    std::wstring& error) {
    std::lock_guard lock(mutex_);
    std::uintmax_t size = 0;
    FILETIME writeTime{};
    if (!ReadFileIdentity(source, size, writeTime)) {
        error = L"历史数据库不存在或不可读。";
        return {};
    }

    auto existing = entries_.find(source);
    if (existing != entries_.end() && existing->second.sourceSize == size &&
        SameFileTime(existing->second.sourceWriteTime, writeTime) &&
        std::filesystem::exists(existing->second.path)) {
        return existing->second.path;
    }
    if (existing != entries_.end()) {
        DeleteSnapshotFamily(existing->second.path);
        entries_.erase(existing);
    }

    const auto destination = SnapshotPathFor(source);
    DeleteSnapshotFamily(destination);
    if (!StreamCopy(source, destination, error)) {
        return {};
    }
    for (const wchar_t* suffix : {L"-wal", L"-shm"}) {
        const std::filesystem::path sidecarSource(source.wstring() + suffix);
        if (std::filesystem::exists(sidecarSource)) {
            std::wstring sidecarError;
            if (!StreamCopy(sidecarSource, std::filesystem::path(destination.wstring() + suffix), sidecarError)) {
                DeleteSnapshotFamily(destination);
                error = std::move(sidecarError);
                return {};
            }
        }
    }
    entries_.emplace(source, Entry{destination, size, writeTime});
    return destination;
}

void SnapshotManager::Clear() {
    std::lock_guard lock(mutex_);
    for (const auto& [source, entry] : entries_) {
        DeleteSnapshotFamily(entry.path);
    }
    entries_.clear();
}

void SnapshotManager::CleanupExpired() {
    std::error_code error;
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    for (const auto& item : std::filesystem::directory_iterator(tempDirectory_, error)) {
        if (error) break;
        const auto filename = item.path().filename().wstring();
        if (!item.is_regular_file(error) || filename.rfind(L"bhl-", 0) != 0) continue;
        const auto time = item.last_write_time(error);
        if (!error && time < cutoff) {
            std::filesystem::remove(item.path(), error);
            error.clear();
        }
    }
}
