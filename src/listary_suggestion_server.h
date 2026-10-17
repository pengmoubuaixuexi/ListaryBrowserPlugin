#pragma once

#include "model.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class ListarySuggestionServer {
public:
    static constexpr std::uint16_t kDefaultPort = 32119;

    explicit ListarySuggestionServer(const AppConfig& config,
        std::filesystem::path workerExecutable = {});
    ~ListarySuggestionServer();

    ListarySuggestionServer(const ListarySuggestionServer&) = delete;
    ListarySuggestionServer& operator=(const ListarySuggestionServer&) = delete;

    bool Start(std::wstring& error);
    std::optional<ListaryAction> ResolveUri(std::wstring_view uri, std::wstring& error) const;
    void InvalidateBluetoothCache();
    // Nonblocking snapshot: no client is being handled and suggestions have
    // been idle for at least idleFor. Health checks do not extend idle time.
    bool CanRecycleHost(std::chrono::milliseconds idleFor) const;
#ifdef BHL_TESTING
    void CacheMappingsForTest(const BrowserDefinition& browser, std::wstring_view prefix,
        std::wstring_view query, const SearchResponse& response);
    void CacheBluetoothMappingsForTest(std::wstring_view prefix, std::wstring_view query,
        const std::vector<BluetoothDeviceTarget>& devices);
    std::vector<std::wstring> CacheAppMappingsForTest(std::wstring_view prefix,
        std::wstring_view query, const std::vector<AppProcessTarget>& targets,
        std::chrono::steady_clock::duration ttl = std::chrono::seconds(30));
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
