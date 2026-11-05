#include "history_search_service.h"

#include "query_parser.h"
#include "search_fallback.h"

#include <algorithm>
#include <chrono>

HistorySearchService::HistorySearchService(ChromiumHistoryAdapter& adapter,
    ChromiumBookmarkAdapter& bookmarkAdapter, SnapshotManager& snapshots, Completion completion)
    : adapter_(adapter), bookmarkAdapter_(bookmarkAdapter), snapshots_(snapshots),
      completion_(std::move(completion)), worker_(&HistorySearchService::WorkerLoop, this) {}

HistorySearchService::~HistorySearchService() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        pending_.reset();
    }
    condition_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void HistorySearchService::Submit(std::vector<BrowserDefinition> browsers, std::wstring query,
    std::size_t limit, std::uint64_t generation, SearchKind kind) {
    latestGeneration_.store(generation, std::memory_order_release);
    {
        std::lock_guard lock(mutex_);
        pending_ = Request{std::move(browsers), std::move(query), limit, generation, kind};
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
    SearchResponse response = request.kind == SearchKind::Bookmarks ?
        ExecuteBookmarks(request) : ExecuteHistory(request);
    response.elapsedMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    return response;
}

SearchResponse HistorySearchService::ExecuteHistory(const Request& request) {
    SearchResponse response;
    response.generation = request.generation;
    if (request.browsers.empty()) return response;
    const auto& browser = request.browsers.front();

    if (IsHttpUrl(request.query) || LooksLikeBrowserAddress(request.query)) {
        HistoryResult direct;
        direct.title = L"使用 " + browser.name + L" 直接打开";
        direct.url = request.query;
        direct.browserId = browser.id;
        direct.browserName = browser.name;
        direct.directUrl = true;
        response.results.push_back(std::move(direct));
    } else {
        const auto profiles = adapter_.DiscoverProfiles(browser);
        if (profiles.empty()) {
            response.error = browser.name + L" 没有可用的历史配置文件。";
        }
        std::wstring lastError;
        for (const auto& profile : profiles) {
            if (request.generation != latestGeneration_.load(std::memory_order_acquire)) break;
            auto partial = adapter_.Search(browser, profile, request.query, request.limit);
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
            if (auto fallback = MakeSearchFallback(browser, request.query)) {
                response.results.push_back(std::move(*fallback));
                response.error.clear();
            }
        }
    }
    return response;
}

SearchResponse HistorySearchService::ExecuteBookmarks(const Request& request) {
    SearchResponse response;
    response.generation = request.generation;

    std::wstring lastError;
    bool anyProfiles = false;
    if (request.browsers.empty()) {
        response.error = L"没有已启用的浏览器，无法搜索书签。";
    }
    for (const auto& browser : request.browsers) {
        if (request.generation != latestGeneration_.load(std::memory_order_acquire)) break;
        const auto profiles = bookmarkAdapter_.DiscoverProfiles(browser);
        for (const auto& profile : profiles) {
            if (request.generation != latestGeneration_.load(std::memory_order_acquire)) break;
            anyProfiles = true;
            auto partial = bookmarkAdapter_.Search(browser, profile, request.query, request.limit);
            ++response.profilesSearched;
            if (!partial.error.empty()) lastError = std::move(partial.error);
            response.results.insert(response.results.end(),
                std::make_move_iterator(partial.results.begin()), std::make_move_iterator(partial.results.end()));
        }
    }
    if (!request.browsers.empty() && !anyProfiles) {
        response.error = L"没有找到可用的浏览器书签文件。";
    }
    std::sort(response.results.begin(), response.results.end(), [](const auto& left, const auto& right) {
        if (left.relevance != right.relevance) return left.relevance < right.relevance;
        if (left.lastVisitTime != right.lastVisitTime) return left.lastVisitTime > right.lastVisitTime;
        if (left.typedCount != right.typedCount) return left.typedCount > right.typedCount;
        return left.visitCount > right.visitCount;
    });
    if (response.results.size() > request.limit) response.results.resize(request.limit);
    if (response.results.empty() && response.error.empty() && !lastError.empty()) {
        response.error = std::move(lastError);
    }
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
