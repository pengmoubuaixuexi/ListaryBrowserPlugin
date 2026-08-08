#pragma once

#include "model.h"
#include "snapshot_manager.h"

#include <string>
#include <string_view>
#include <mutex>
#include <set>
#include <vector>

struct AdapterSearchResult {
    std::vector<HistoryResult> results;
    std::wstring error;
    bool schemaIncompatible = false;
};

class IBrowserHistoryAdapter {
public:
    virtual std::vector<BrowserProfile> DiscoverProfiles(const BrowserDefinition& browser) = 0;
    virtual AdapterSearchResult Search(const BrowserDefinition& browser, const BrowserProfile& profile,
        std::wstring_view query, std::size_t limit) = 0;
    virtual ~IBrowserHistoryAdapter() = default;
};

class ChromiumHistoryAdapter final : public IBrowserHistoryAdapter {
public:
    explicit ChromiumHistoryAdapter(SnapshotManager& snapshots) : snapshots_(snapshots) {}

    std::vector<BrowserProfile> DiscoverProfiles(const BrowserDefinition& browser) override;
    AdapterSearchResult Search(const BrowserDefinition& browser, const BrowserProfile& profile,
        std::wstring_view query, std::size_t limit) override;

private:
    static std::vector<std::wstring> ReadLocalStateProfiles(const std::filesystem::path& userDataRoot);
    static AdapterSearchResult SearchDatabase(const BrowserDefinition& browser,
        const BrowserProfile& profile, const std::filesystem::path& database,
        std::wstring_view query, std::size_t limit);

    SnapshotManager& snapshots_;
    std::mutex preferenceMutex_;
    std::set<std::filesystem::path> preferSnapshots_;
};
