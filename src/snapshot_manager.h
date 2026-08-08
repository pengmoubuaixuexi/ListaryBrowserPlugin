#pragma once

#include <Windows.h>

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <cstdint>

class SnapshotManager {
public:
    SnapshotManager();
    ~SnapshotManager();

    std::filesystem::path CreateOrReuse(const std::filesystem::path& source, std::wstring& error);
    void Clear();

private:
    struct Entry {
        std::filesystem::path path;
        std::uintmax_t sourceSize = 0;
        FILETIME sourceWriteTime{};
    };

    static bool StreamCopy(const std::filesystem::path& source, const std::filesystem::path& destination,
        std::wstring& error);
    void CleanupExpired();
    std::filesystem::path SnapshotPathFor(const std::filesystem::path& source) const;

    std::filesystem::path tempDirectory_;
    std::uint64_t instanceId_ = 0;
    std::map<std::filesystem::path, Entry> entries_;
    std::mutex mutex_;
};
