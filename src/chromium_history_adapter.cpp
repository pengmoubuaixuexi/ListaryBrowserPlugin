#include "chromium_history_adapter.h"

#include "text_util.h"

#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

namespace {
struct DatabaseCloser {
    void operator()(sqlite3* db) const { if (db) sqlite3_close(db); }
};
struct StatementCloser {
    void operator()(sqlite3_stmt* statement) const { if (statement) sqlite3_finalize(statement); }
};
using DatabasePtr = std::unique_ptr<sqlite3, DatabaseCloser>;
using StatementPtr = std::unique_ptr<sqlite3_stmt, StatementCloser>;

std::wstring ReadSqliteText16(sqlite3_stmt* statement, int column) {
    const auto* text = static_cast<const wchar_t*>(sqlite3_column_text16(statement, column));
    const int bytes = sqlite3_column_bytes16(statement, column);
    return text && bytes > 0 ? std::wstring(text, static_cast<std::size_t>(bytes) / sizeof(wchar_t)) : std::wstring();
}

enum class TableStatus { Present, Missing, ReadError };

TableStatus CheckTable(sqlite3* db, const char* table) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", -1, &raw, nullptr) != SQLITE_OK) {
        return TableStatus::ReadError;
    }
    StatementPtr statement(raw);
    sqlite3_bind_text(statement.get(), 1, table, -1, SQLITE_TRANSIENT);
    const int status = sqlite3_step(statement.get());
    if (status == SQLITE_ROW) return TableStatus::Present;
    if (status == SQLITE_DONE) return TableStatus::Missing;
    return TableStatus::ReadError;
}

std::set<std::string> Columns(sqlite3* db, const char* table, bool& complete) {
    complete = false;
    std::set<std::string> result;
    const std::string sql = std::string("PRAGMA table_info(") + table + ")";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) return result;
    StatementPtr statement(raw);
    int status = SQLITE_OK;
    while ((status = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
        if (name) result.insert(name);
    }
    complete = status == SQLITE_DONE;
    return result;
}

std::wstring EscapeLike(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch == L'%' || ch == L'_' || ch == L'\\') result.push_back(L'\\');
        result.push_back(ch);
    }
    return result;
}

std::wstring DecodeJsonString(std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            decoded.push_back(text[i]);
            continue;
        }
        const char escaped = text[++i];
        switch (escaped) {
        case '\"': decoded.push_back('\"'); break;
        case '\\': decoded.push_back('\\'); break;
        case '/': decoded.push_back('/'); break;
        case 'b': decoded.push_back('\b'); break;
        case 'f': decoded.push_back('\f'); break;
        case 'n': decoded.push_back('\n'); break;
        case 'r': decoded.push_back('\r'); break;
        case 't': decoded.push_back('\t'); break;
        default: return {};
        }
    }
    return Utf8ToWide(decoded);
}

std::size_t SkipJsonString(const std::string& json, std::size_t quote, std::string* contents = nullptr) {
    std::size_t i = quote + 1;
    const std::size_t start = i;
    bool escaped = false;
    for (; i < json.size(); ++i) {
        if (!escaped && json[i] == '\"') {
            if (contents) *contents = json.substr(start, i - start);
            return i + 1;
        }
        if (!escaped && json[i] == '\\') escaped = true;
        else escaped = false;
    }
    return json.size();
}
}

std::vector<std::wstring> ChromiumHistoryAdapter::ReadLocalStateProfiles(
    const std::filesystem::path& userDataRoot) {
    const auto path = userDataRoot / L"Local State";
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    stream.seekg(0, std::ios::end);
    const auto length = stream.tellg();
    if (length <= 0 || length > 8 * 1024 * 1024) return {};
    stream.seekg(0);
    std::string json(static_cast<std::size_t>(length), '\0');
    stream.read(json.data(), length);
    if (!stream) return {};

    const auto marker = json.find("\"info_cache\"");
    if (marker == std::string::npos) return {};
    const auto objectStart = json.find('{', marker);
    if (objectStart == std::string::npos) return {};

    std::vector<std::wstring> profiles;
    int depth = 1;
    for (std::size_t i = objectStart + 1; i < json.size() && depth > 0;) {
        if (json[i] == '\"') {
            std::string raw;
            const auto next = SkipJsonString(json, i, depth == 1 ? &raw : nullptr);
            if (next >= json.size()) break;
            std::size_t after = next;
            while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after]))) ++after;
            if (depth == 1 && after < json.size() && json[after] == ':') {
                const auto decoded = DecodeJsonString(raw);
                if (!decoded.empty()) profiles.push_back(decoded);
            }
            i = next;
        } else {
            if (json[i] == '{' || json[i] == '[') ++depth;
            else if (json[i] == '}' || json[i] == ']') --depth;
            ++i;
        }
    }
    return profiles;
}

std::vector<BrowserProfile> ChromiumHistoryAdapter::DiscoverProfiles(const BrowserDefinition& browser) {
    std::filesystem::path userDataRoot;
    for (const auto& candidate : browser.userDataCandidates) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate, error)) {
            userDataRoot = candidate;
            break;
        }
    }
    if (userDataRoot.empty()) return {};

    auto names = ReadLocalStateProfiles(userDataRoot);
    if (names.empty()) {
        std::error_code error;
        for (const auto& item : std::filesystem::directory_iterator(userDataRoot, error)) {
            if (error || !item.is_directory(error)) continue;
            const auto name = item.path().filename().wstring();
            if (name == L"Default" || StartsWithInsensitive(name, L"Profile ")) names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    std::vector<BrowserProfile> profiles;
    for (const auto& name : names) {
        if (!browser.enabledProfiles.empty() && browser.enabledProfiles.front() != L"*" &&
            std::find_if(browser.enabledProfiles.begin(), browser.enabledProfiles.end(), [&](const auto& enabled) {
                return CompareStringOrdinal(enabled.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL;
            }) == browser.enabledProfiles.end()) {
            continue;
        }
        const auto history = userDataRoot / name / browser.historyRelativePath;
        std::error_code error;
        if (!std::filesystem::is_regular_file(history, error)) continue;
        profiles.push_back(BrowserProfile{browser.id, name, name, userDataRoot, history});
    }
    return profiles;
}

AdapterSearchResult ChromiumHistoryAdapter::SearchDatabase(const BrowserDefinition& browser,
    const BrowserProfile& profile, const std::filesystem::path& database,
    std::wstring_view query, std::size_t limit) {
    AdapterSearchResult result;
    sqlite3* rawDatabase = nullptr;
    const auto utf8Path = WideToUtf8(database.wstring());
    const int openStatus = sqlite3_open_v2(utf8Path.c_str(), &rawDatabase,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    DatabasePtr db(rawDatabase);
    if (openStatus != SQLITE_OK || !db) {
        result.error = L"无法只读打开历史数据库。";
        return result;
    }
    sqlite3_busy_timeout(db.get(), 25);
    const auto tableStatus = CheckTable(db.get(), "urls");
    if (tableStatus == TableStatus::ReadError) {
        result.error = L"历史数据库正忙或读取失败。";
        return result;
    }
    if (tableStatus == TableStatus::Missing) {
        result.schemaIncompatible = true;
        result.error = L"浏览器历史格式暂不支持。";
        return result;
    }
    bool columnsComplete = false;
    const auto columns = Columns(db.get(), "urls", columnsComplete);
    if (!columnsComplete) {
        result.error = L"历史数据库正忙或读取失败。";
        return result;
    }
    for (const char* required : {"url", "title", "last_visit_time"}) {
        if (!columns.contains(required)) {
            result.schemaIncompatible = true;
            result.error = L"浏览器历史格式暂不支持。";
            return result;
        }
    }
    const std::string typed = columns.contains("typed_count") ? "typed_count" : "0";
    const std::string visits = columns.contains("visit_count") ? "visit_count" : "0";

    std::string sql = "SELECT url, COALESCE(title,''), last_visit_time, " + typed +
        " AS typed_score, " + visits + " AS visit_score, ";
    if (query.empty()) {
        sql += "0 AS relevance FROM urls WHERE url IS NOT NULL AND url <> '' "
               "ORDER BY last_visit_time DESC, typed_score DESC, visit_score DESC LIMIT ?1";
    } else {
        sql += "CASE WHEN title = ?1 COLLATE NOCASE THEN 0 "
               "WHEN title LIKE ?2 ESCAPE '\\' COLLATE NOCASE THEN 1 "
               "WHEN url LIKE ?2 ESCAPE '\\' COLLATE NOCASE THEN 2 ELSE 3 END AS relevance "
               "FROM urls WHERE (title LIKE ?3 ESCAPE '\\' COLLATE NOCASE "
               "OR url LIKE ?3 ESCAPE '\\' COLLATE NOCASE) "
               "ORDER BY relevance, last_visit_time DESC, typed_score DESC, visit_score DESC LIMIT ?4";
    }

    sqlite3_stmt* rawStatement = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &rawStatement, nullptr) != SQLITE_OK) {
        result.error = L"历史查询准备失败。";
        return result;
    }
    StatementPtr statement(rawStatement);
    if (query.empty()) {
        sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(limit));
    } else {
        const std::wstring escaped = EscapeLike(query);
        const std::wstring prefix = escaped + L"%";
        const std::wstring contains = L"%" + escaped + L"%";
        sqlite3_bind_text16(statement.get(), 1, query.data(), static_cast<int>(query.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
        sqlite3_bind_text16(statement.get(), 2, prefix.data(), static_cast<int>(prefix.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
        sqlite3_bind_text16(statement.get(), 3, contains.data(), static_cast<int>(contains.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(limit));
    }

    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        HistoryResult item;
        item.url = ReadSqliteText16(statement.get(), 0);
        item.title = ReadSqliteText16(statement.get(), 1);
        if (item.title.empty()) item.title = item.url;
        item.lastVisitTime = sqlite3_column_int64(statement.get(), 2);
        item.typedCount = sqlite3_column_int(statement.get(), 3);
        item.visitCount = sqlite3_column_int(statement.get(), 4);
        item.relevance = sqlite3_column_int(statement.get(), 5);
        item.browserId = browser.id;
        item.browserName = browser.name;
        item.profileDirectory = profile.directoryName;
        result.results.push_back(std::move(item));
    }
    if (step != SQLITE_DONE) {
        result.results.clear();
        result.error = L"历史数据库正忙或读取失败。";
    }
    return result;
}

AdapterSearchResult ChromiumHistoryAdapter::Search(const BrowserDefinition& browser,
    const BrowserProfile& profile, std::wstring_view query, std::size_t limit) {
    bool preferSnapshot = false;
    {
        std::lock_guard lock(preferenceMutex_);
        preferSnapshot = preferSnapshots_.contains(profile.historyPath);
    }
    if (preferSnapshot) {
        std::wstring snapshotError;
        const auto snapshot = snapshots_.CreateOrReuse(profile.historyPath, snapshotError);
        if (!snapshot.empty()) {
            auto cached = SearchDatabase(browser, profile, snapshot, query, limit);
            if (cached.error.empty() || cached.schemaIncompatible) return cached;
        }
        std::lock_guard lock(preferenceMutex_);
        preferSnapshots_.erase(profile.historyPath);
    }

    auto direct = SearchDatabase(browser, profile, profile.historyPath, query, limit);
    if (direct.error.empty() || direct.schemaIncompatible) return direct;

    std::wstring snapshotError;
    const auto snapshot = snapshots_.CreateOrReuse(profile.historyPath, snapshotError);
    if (snapshot.empty()) {
        if (!snapshotError.empty()) direct.error = std::move(snapshotError);
        return direct;
    }
    auto fallback = SearchDatabase(browser, profile, snapshot, query, limit);
    if (fallback.error.empty()) {
        std::lock_guard lock(preferenceMutex_);
        preferSnapshots_.insert(profile.historyPath);
    }
    return fallback;
}
