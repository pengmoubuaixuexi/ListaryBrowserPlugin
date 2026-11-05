#include "chromium_bookmark_adapter.h"

#include "json_document.h"
#include "text_util.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <utility>

namespace {
constexpr std::uintmax_t kMaxBookmarkFileBytes = 16 * 1024 * 1024;

std::wstring StringProperty(const JsonValue& object, std::wstring_view name) {
    const JsonValue* value = object.Find(name);
    return value && value->type() == JsonValue::Type::String ? value->text() : L"";
}

// Chromium stores timestamps as decimal strings of microseconds since
// 1601-01-01, the same epoch as the history database's last_visit_time.
std::int64_t ParseChromiumTimestamp(const std::wstring& value) {
    if (value.empty()) return 0;
    wchar_t* end = nullptr;
    const long long parsed = wcstoll(value.c_str(), &end, 10);
    return end == value.c_str() ? 0 : parsed;
}

// Mirrors the history adapter's relevance tiers: exact title, title prefix,
// title contains, URL contains. Returns -1 when the entry does not match.
int MatchRelevance(const std::wstring& title, const std::wstring& url,
    const std::wstring& normalizedQuery) {
    if (normalizedQuery.empty()) return 0;
    const std::wstring normalizedTitle = ToLowerInvariant(title);
    if (normalizedTitle == normalizedQuery) return 0;
    if (normalizedTitle.rfind(normalizedQuery, 0) == 0) return 1;
    if (normalizedTitle.find(normalizedQuery) != std::wstring::npos) return 2;
    if (ToLowerInvariant(url).find(normalizedQuery) != std::wstring::npos) return 3;
    return -1;
}

void CollectBookmarkNodes(const JsonValue::Array& children, const BrowserDefinition& browser,
    const BrowserProfile& profile, const std::wstring& normalizedQuery,
    std::vector<HistoryResult>& out) {
    for (const auto& child : children) {
        if (child.type() != JsonValue::Type::Object) continue;
        const auto& nodeType = StringProperty(child, L"type");
        if (nodeType == L"folder") {
            const JsonValue* nested = child.Find(L"children");
            if (nested && nested->type() == JsonValue::Type::Array) {
                CollectBookmarkNodes(nested->array(), browser, profile, normalizedQuery, out);
            }
            continue;
        }
        if (nodeType != L"url") continue;
        HistoryResult item;
        item.url = StringProperty(child, L"url");
        if (item.url.empty()) continue;
        item.title = StringProperty(child, L"name");
        if (item.title.empty()) item.title = item.url;
        item.lastVisitTime = ParseChromiumTimestamp(StringProperty(child, L"date_added"));
        const int relevance = MatchRelevance(item.title, item.url, normalizedQuery);
        if (relevance < 0) continue;
        item.relevance = relevance;
        item.browserId = browser.id;
        item.browserName = browser.name;
        item.profileDirectory = profile.directoryName;
        item.bookmark = true;
        out.push_back(std::move(item));
    }
}
}

std::vector<BrowserProfile> ChromiumBookmarkAdapter::DiscoverProfiles(const BrowserDefinition& browser) {
    return ChromiumHistoryAdapter::DiscoverProfilesForFile(browser, L"Bookmarks");
}

AdapterSearchResult ChromiumBookmarkAdapter::Search(const BrowserDefinition& browser,
    const BrowserProfile& profile, std::wstring_view query, std::size_t limit) {
    AdapterSearchResult result;
    const auto bookmarksPath = profile.userDataRoot / profile.directoryName / L"Bookmarks";
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(bookmarksPath, fileError)) {
        result.error = L"书签文件不存在。";
        return result;
    }
    std::ifstream stream(bookmarksPath, std::ios::binary);
    if (!stream) {
        result.error = L"无法读取书签文件。";
        return result;
    }
    stream.seekg(0, std::ios::end);
    const auto length = stream.tellg();
    if (length <= 0) {
        result.error = L"书签文件为空。";
        return result;
    }
    if (static_cast<std::uintmax_t>(length) > kMaxBookmarkFileBytes) {
        result.error = L"书签文件过大，暂不支持。";
        return result;
    }
    stream.seekg(0);
    std::string bytes(static_cast<std::size_t>(length), '\0');
    stream.read(bytes.data(), length);
    if (!stream) {
        result.error = L"无法读取书签文件。";
        return result;
    }

    JsonValue root;
    std::wstring parseError;
    if (!ParseJsonUtf8(bytes, root, parseError)) {
        result.error = L"书签文件无法解析。";
        return result;
    }
    const JsonValue* roots = root.Find(L"roots");
    if (!roots || roots->type() != JsonValue::Type::Object) {
        result.error = L"浏览器书签格式暂不支持。";
        return result;
    }

    const std::wstring normalizedQuery = ToLowerInvariant(Trim(query));
    std::vector<HistoryResult> matched;
    for (const auto& tree : roots->object()) {
        if (tree.second.type() != JsonValue::Type::Object) continue;
        const JsonValue* children = tree.second.Find(L"children");
        if (!children || children->type() != JsonValue::Type::Array) continue;
        CollectBookmarkNodes(children->array(), browser, profile, normalizedQuery, matched);
    }
    std::sort(matched.begin(), matched.end(), [](const auto& left, const auto& right) {
        if (left.relevance != right.relevance) return left.relevance < right.relevance;
        return left.lastVisitTime > right.lastVisitTime;
    });
    if (matched.size() > limit) matched.resize(limit);
    result.results = std::move(matched);
    return result;
}
