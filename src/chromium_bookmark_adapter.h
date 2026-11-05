#pragma once

#include "chromium_history_adapter.h"
#include "model.h"

#include <string>
#include <string_view>
#include <vector>

// Reads the per-profile Chromium "Bookmarks" JSON file. Unlike the history
// database there is no locking concern: the file is loaded with one ifstream
// pass, so no SnapshotManager or snapshot preference state is needed.
class ChromiumBookmarkAdapter {
public:
    std::vector<BrowserProfile> DiscoverProfiles(const BrowserDefinition& browser);
    AdapterSearchResult Search(const BrowserDefinition& browser, const BrowserProfile& profile,
        std::wstring_view query, std::size_t limit);
};
