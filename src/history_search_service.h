#pragma once

#include "chromium_history_adapter.h"
#include "model.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

class HistorySearchService {
public:
    using Completion = std::function<void(SearchResponse&&)>;

    HistorySearchService(ChromiumHistoryAdapter& adapter, SnapshotManager& snapshots, Completion completion);
    ~HistorySearchService();

    void Submit(const BrowserDefinition& browser, std::wstring query,
        std::size_t limit, std::uint64_t generation);
    void CancelAndCleanup(std::uint64_t generation);

private:
    struct Request {
        BrowserDefinition browser;
        std::wstring query;
        std::size_t limit = 20;
        std::uint64_t generation = 0;
    };

    void WorkerLoop();
    SearchResponse Execute(const Request& request);

    ChromiumHistoryAdapter& adapter_;
    SnapshotManager& snapshots_;
    Completion completion_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Request> pending_;
    bool cleanupRequested_ = false;
    bool stopping_ = false;
    std::atomic<std::uint64_t> latestGeneration_{0};
};
