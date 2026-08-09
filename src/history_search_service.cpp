#include "history_search_service.h"

#include "query_parser.h"
#include "search_fallback.h"

#include <algorithm>
#include <chrono>

HistorySearchService::HistorySearchService(ChromiumHistoryAdapter& adapter,
    SnapshotManager& snapshots, Completion completion)
    : adapter_(adapter), snapshots_(snapshots), completion_(std::move(completion)),
      worker_(&HistorySearchService::WorkerLoop, this) {}

HistorySearchService::~HistorySearchService() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        pending_.reset();
    }
    condition_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void HistorySearchService::Submit(const BrowserDefinition& browser, std::wstring query,
    std::size_t limit, std::uint64_t generation) {
    latestGeneration_.store(generation, std::memory_order_release);
    {
        std::lock_guard lock(mutex_);
        pending_ = Request{browser, std::move(query), limit, generation};
        cleanupRequested_ = false;
    }
    condition_.notify_one();
}

void HistorySearchService::CancelAndCleanup(std::uint64_t generation) {
    latestGeneration_.store(generation, std::memory_order_release);
    {
        std::lock_guard lock(mutex_);
        pending_.reset();
        cleanupRequested_ = true;
    }
    condition_.notify_one();
}

SearchResponse HistorySearchService::Execute(const Request& request) {
    const auto started = std::chrono::steady_clock::now();
    SearchResponse response;
    response.generation = request.generation;

    if (IsHttpUrl(request.query) || LooksLikeBrowserAddress(request.query)) {
        HistoryResult direct;
        direct.title = L"使用 " + request.browser.name + L" 直接打开";
        direct.url = request.query;
        direct.browserId = request.browser.id;
        direct.browserName = request.browser.name;
        direct.directUrl = true;
        response.results.push_back(std::move(direct));
    } else {
        const auto profiles = adapter_.DiscoverProfiles(request.browser);
        if (profiles.empty()) {
            response.error = request.browser.name + L" 没有可用的历史配置文件。";
        }
        std::wstring lastError;
        for (const auto& profile : profiles) {
            if (request.generation != latestGeneration_.load(std::memory_order_acquire)) break;
            auto partial = adapter_.Search(request.browser, profile, request.query, request.limit);
            ++response.profilesSearched;
            if (!partial.error.empty()) lastError = std::move(partial.error);
            response.results.insert(response.results.end(),
                std::make_move_iterator(partial.results.begin()), std::make_move_iterator(partial.results.end()));
        }
        std::sort(response.results.begin(), response.results.end(), [](const auto& left, const auto& right) {
            if (left.relevance != right.relevance) return left.relevance < right.relevance;
            if (left.lastVisitTime != right.lastVisitTime) return left.lastVisitTime > right.lastVisitTime;
            if (left.typedCount != right.typedCount) return left.typedCount > right.typedCount;
            return left.visitCount > right.visitCount;
        });
        if (response.results.size() > request.limit) response.results.resize(request.limit);
        if (response.results.empty() && response.error.empty() && !lastError.empty()) response.error = std::move(lastError);
        if (response.results.empty()) {
            if (auto fallback = MakeSearchFallback(request.browser, request.query)) {
                response.results.push_back(std::move(*fallback));
                response.error.clear();
            }
        }
    }

    response.elapsedMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    return response;
}

void HistorySearchService::WorkerLoop() {
    for (;;) {
        std::optional<Request> request;
        bool cleanup = false;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return stopping_ || pending_.has_value() || cleanupRequested_; });
            if (stopping_) break;
            request = std::move(pending_);
            pending_.reset();
            cleanup = cleanupRequested_;
            cleanupRequested_ = false;
        }
        if (cleanup) snapshots_.Clear();
        if (!request) continue;
        auto response = Execute(*request);
        if (response.generation == latestGeneration_.load(std::memory_order_acquire)) {
            completion_(std::move(response));
        }
    }
    snapshots_.Clear();
}
