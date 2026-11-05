#include "browser_discovery.h"
#include "app_termination_tests.h"
#include "browser_icon.h"
#include "browser_launcher.h"
#include "chromium_bookmark_adapter.h"
#include "chromium_history_adapter.h"
#include "config_store.h"
#include "json_document.h"
#include "listary_configurator.h"
#include "listary_suggestion_server.h"
#include "query_parser.h"
#include "search_fallback.h"
#include "snapshot_manager.h"
#include "text_util.h"

#include <Windows.h>
#include <winsqlite/winsqlite3.h>

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    if (!condition) ++failures;
}

bool Exec(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int status = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (message) sqlite3_free(message);
    return status == SQLITE_OK;
}

bool CreateHistory(const std::filesystem::path& path, int rows, bool fullSchema) {
    std::filesystem::create_directories(path.parent_path());
    sqlite3* db = nullptr;
    const std::string utf8Path = WideToUtf8(path.wstring());
    if (sqlite3_open_v2(utf8Path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    const char* schema = fullSchema
        ? "CREATE TABLE urls(id INTEGER PRIMARY KEY,url LONGVARCHAR,title LONGVARCHAR,visit_count INTEGER DEFAULT 0,typed_count INTEGER DEFAULT 0,last_visit_time INTEGER NOT NULL DEFAULT 0);"
        : "CREATE TABLE urls(id INTEGER PRIMARY KEY,url LONGVARCHAR,title LONGVARCHAR);";
    bool ok = Exec(db, schema) && Exec(db, "BEGIN IMMEDIATE");
    sqlite3_stmt* insert = nullptr;
    if (ok && fullSchema) {
        ok = sqlite3_prepare_v2(db,
            "INSERT INTO urls(url,title,visit_count,typed_count,last_visit_time) VALUES(?1,?2,?3,?4,?5)",
            -1, &insert, nullptr) == SQLITE_OK;
    }
    for (int i = 0; ok && i < rows; ++i) {
        const bool special = i == rows / 2;
        const std::string url = special ? "https://example.test/%E4%B8%AD%E6%96%87/needle" :
            "https://site" + std::to_string(i % 1000) + ".example/path/" + std::to_string(i);
        const std::string title = special ? "中文 Needle 页面" : "History item " + std::to_string(i);
        sqlite3_bind_text(insert, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 2, title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert, 3, i % 20);
        sqlite3_bind_int(insert, 4, i % 5);
        sqlite3_bind_int64(insert, 5, 13300000000000000LL + i);
        ok = sqlite3_step(insert) == SQLITE_DONE;
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
    }
    if (insert) sqlite3_finalize(insert);
    ok = ok && Exec(db, "COMMIT");
    sqlite3_close(db);
    return ok;
}

bool WriteUtf8File(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(output);
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::size_t CountBhlInsertions(const JsonValue& root) {
    const auto* webSearch = root.Find(L"WebSearch");
    const auto* items = webSearch ? webSearch->Find(L"Items") : nullptr;
    const auto* insertions = items ? items->Find(L"Insertions") : nullptr;
    if (!insertions || insertions->type() != JsonValue::Type::Array) return 0;
    std::size_t count = 0;
    for (const auto& insertion : insertions->array()) {
        const auto* item = insertion.Find(L"Item");
        const auto* url = item ? item->Find(L"Url") : nullptr;
        if (url && url->type() == JsonValue::Type::String && StartsWithInsensitive(url->text(), L"bhl://")) ++count;
    }
    return count;
}

std::size_t CountDisabledGoogleUpdates(const JsonValue& root) {
    const auto* webSearch = root.Find(L"WebSearch");
    const auto* items = webSearch ? webSearch->Find(L"Items") : nullptr;
    const auto* updates = items ? items->Find(L"Updates") : nullptr;
    if (!updates || updates->type() != JsonValue::Type::Array) return 0;
    std::size_t count = 0;
    for (const auto& update : updates->array()) {
        const auto* id = update.Find(L"Id");
        const auto* properties = update.Find(L"UpdatedProperties");
        const auto* enabled = properties ? properties->Find(L"Enabled") : nullptr;
        if (id && id->type() == JsonValue::Type::String &&
            id->text() == L"ecb51462-cb27-4b89-ae53-333b4550f489" &&
            enabled && enabled->type() == JsonValue::Type::Boolean && !enabled->boolean()) ++count;
    }
    return count;
}

std::wstring BhlIconPath(const JsonValue& root, std::wstring_view keyword) {
    const auto* webSearch = root.Find(L"WebSearch");
    const auto* items = webSearch ? webSearch->Find(L"Items") : nullptr;
    const auto* insertions = items ? items->Find(L"Insertions") : nullptr;
    if (!insertions || insertions->type() != JsonValue::Type::Array) return {};
    for (const auto& insertion : insertions->array()) {
        const auto* item = insertion.Find(L"Item");
        const auto* itemKeyword = item ? item->Find(L"Keyword") : nullptr;
        const auto* icon = item ? item->Find(L"Icon") : nullptr;
        const auto* path = icon ? icon->Find(L"Path") : nullptr;
        if (itemKeyword && itemKeyword->type() == JsonValue::Type::String &&
            itemKeyword->text() == keyword && path && path->type() == JsonValue::Type::String) {
            return path->text();
        }
    }
    return {};
}
}

int wmain(int argc, wchar_t** argv) {
    int helperExitCode = 0;
    if (TryRunAppTerminationTestHelper(argc, argv, helperExitCode)) return helperExitCode;
    wchar_t captureBuffer[32768]{};
    if (GetEnvironmentVariableW(L"BHL_TEST_CAPTURE", captureBuffer, 32768) > 0 && argc >= 2) {
        std::ofstream capture(std::filesystem::path(captureBuffer), std::ios::binary);
        if (argc >= 3) capture << WideToUtf8(argv[1]) << '\n' << WideToUtf8(argv[2]) << '\n';
        else capture << '\n' << WideToUtf8(argv[1]) << '\n';
        return capture ? 0 : 3;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--bhl-test-hold") == 0) {
        Sleep(30000);
        return 0;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--bhl-test-visible") == 0) {
        HWND window = CreateWindowExW(0, L"STATIC", L"Listary test busy window",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, -32000, -32000, 100, 100,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) return 4;
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return 0;
    }

    const auto parsed = ParseQuery(L"G  github");
    Check(!parsed.empty && parsed.prefix == L"g" && parsed.query == L"github", "query parser is case-insensitive");
    Check(IsHttpUrl(L"HTTPS://example.com/a b"), "HTTP URL detection");
    Check(BrowserLauncher::QuoteArgument(L"a b\\\"c") == L"\"a b\\\\\\\"c\"", "CreateProcess argument quoting");
    BrowserDefinition genericSearchBrowser;
    genericSearchBrowser.id = L"q";
    genericSearchBrowser.name = L"Quark";
    const auto genericFallback = MakeSearchFallback(genericSearchBrowser, L"百度 a&b");
    Check(genericFallback && genericFallback->url == L"? 百度 a&b" &&
        genericFallback->title == L"在 Quark 地址栏搜索“百度 a&b”",
        "generic zero-history fallback delegates text to browser omnibox search");
    const auto addressFallback = MakeSearchFallback(genericSearchBrowser, L"baidu.com");
    Check(LooksLikeBrowserAddress(L"baidu.com") && addressFallback &&
        addressFallback->url == L"baidu.com" &&
        addressFallback->title == L"使用 Quark 打开 baidu.com",
        "bare domain is delegated to browser URL fixup without a search engine URL");
    Check(!MakeSearchFallback(genericSearchBrowser, L""),
        "empty query does not create a web-search fallback");
    AppConfig mappingConfig;
    mappingConfig.browsers = {genericSearchBrowser};
    ListarySuggestionServer mappingServer(mappingConfig);
    SearchResponse mappingResponse;
    HistoryResult firstHistory;
    firstHistory.title = L"百度校园招聘";
    firstHistory.url = L"history-target";
    firstHistory.browserId = genericSearchBrowser.id;
    firstHistory.browserName = genericSearchBrowser.name;
    mappingResponse.results.push_back(firstHistory);
    mappingServer.CacheMappingsForTest(genericSearchBrowser, L"q", L"百度", mappingResponse);
    std::wstring mappingError;
    const auto rawQuerySelection = mappingServer.ResolveUri(
        L"bhl://open?prefix=q&selection=百度", mappingError);
    const auto historySelection = mappingServer.ResolveUri(
        L"bhl://open?prefix=q&selection=百度校园招聘", mappingError);
    Check(rawQuerySelection && rawQuerySelection->kind == ListaryActionKind::BrowserOpen &&
        rawQuerySelection->browserResult.url == L"? 百度",
        "Listary raw-query first row always maps to browser omnibox search");
    Check(historySelection && historySelection->kind == ListaryActionKind::BrowserOpen &&
        historySelection->browserResult.url == L"history-target",
        "Listary history rows keep their own URL mappings");
    BluetoothDeviceTarget bluetoothTarget;
    bluetoothTarget.stableKey = L"address:test";
    bluetoothTarget.displayName = L"AirPods";
    mappingServer.CacheBluetoothMappingsForTest(L"ly", L"air", {bluetoothTarget});
    const auto bluetoothSelection = mappingServer.ResolveUri(
        L"bhl://bluetooth?prefix=ly&selection=AirPods%20%20%E2%80%94%20%20%E6%9C%AA%E8%BF%9E%E6%8E%A5", mappingError);
    Check(bluetoothSelection && bluetoothSelection->kind == ListaryActionKind::BluetoothConnect &&
        bluetoothSelection->bluetoothTarget.stableKey == bluetoothTarget.stableKey,
        "Listary Bluetooth suggestion maps to an internal device target");
    HistoryResult bookmarkResult;
    bookmarkResult.title = L"GitHub";
    bookmarkResult.url = L"https://github.com/";
    bookmarkResult.browserId = genericSearchBrowser.id;
    bookmarkResult.browserName = genericSearchBrowser.name;
    bookmarkResult.profileDirectory = L"Profile 1";
    bookmarkResult.bookmark = true;
    SearchResponse bookmarkResponse;
    bookmarkResponse.results.push_back(bookmarkResult);
    const auto bookmarkSuggestions = mappingServer.CacheBookmarkMappingsForTest(
        L"bm", L"git", bookmarkResponse);
    const auto bookmarkSelection = mappingServer.ResolveUri(
        L"bhl://bookmark?prefix=bm&selection=GitHub%20%20%E2%80%94%20%20github.com", mappingError);
    Check(bookmarkSuggestions.size() == 1 && bookmarkSelection &&
        bookmarkSelection->kind == ListaryActionKind::BrowserOpen &&
        bookmarkSelection->browserResult.url == L"https://github.com/" &&
        bookmarkSelection->browserResult.profileDirectory == L"Profile 1",
        "Listary bookmark rows keep their own URL and profile mappings");
    const auto bookmarkQuerySelection = mappingServer.ResolveUri(
        L"bhl://bookmark?prefix=bm&selection=git", mappingError);
    Check(bookmarkQuerySelection && bookmarkQuerySelection->kind == ListaryActionKind::BrowserOpen &&
        bookmarkQuerySelection->browserResult.url == L"https://github.com/",
        "Listary bookmark raw-query row opens the top bookmark result");

    // Review regression: a bookmark whose display text equals the raw query
    // must stay independently selectable from the raw-query row.
    HistoryResult edgeBookmark;
    edgeBookmark.title = L"example.test";
    edgeBookmark.url = L"https://example.test/saved/deep-page";
    edgeBookmark.browserId = L"edge";
    edgeBookmark.browserName = L"Microsoft Edge";
    edgeBookmark.profileDirectory = L"Profile 1";
    edgeBookmark.bookmark = true;
    SearchResponse sameTextResponse;
    sameTextResponse.results.push_back(edgeBookmark);
    const auto sameTextSuggestions = mappingServer.CacheBookmarkMappingsForTest(
        L"bm2", L"example.test", sameTextResponse);
    Check(sameTextSuggestions.size() == 1 &&
        sameTextSuggestions.front() == L"example.test · 书签",
        "bookmark display equal to the query gets a distinct marker suffix");
    const auto markedSelection = mappingServer.ResolveUri(
        L"bhl://bookmark?prefix=bm2&selection=example.test%20%C2%B7%20%E4%B9%A6%E7%AD%BE", mappingError);
    const auto rawRowSelection = mappingServer.ResolveUri(
        L"bhl://bookmark?prefix=bm2&selection=example.test", mappingError);
    Check(markedSelection && markedSelection->browserResult.url == L"https://example.test/saved/deep-page" &&
        markedSelection->browserResult.browserId == L"edge" &&
        markedSelection->browserResult.profileDirectory == L"Profile 1",
        "bookmark row named like the query keeps its own URL, browser and profile");
    Check(rawRowSelection && rawRowSelection->browserResult.url == L"https://example.test/saved/deep-page" &&
        rawRowSelection->browserResult.browserId == L"edge",
        "raw-query row resolves independently without overwriting the bookmark");

    // Review regression: literal "(2)"-style bookmark names must not collide
    // with generated duplicate numbering; every suggestion stays unique.
    auto makeDocs = [](const wchar_t* title, const wchar_t* url) {
        HistoryResult item;
        item.title = title;
        item.url = url;
        item.browserId = L"edge";
        item.browserName = L"Microsoft Edge";
        item.bookmark = true;
        return item;
    };
    SearchResponse numberedResponse;
    numberedResponse.results.push_back(makeDocs(L"Docs", L"about:blank#0"));
    numberedResponse.results.push_back(makeDocs(L"Docs", L"about:blank#1"));
    numberedResponse.results.push_back(makeDocs(L"Docs (2)", L"about:blank#2"));
    const auto numberedSuggestions = mappingServer.CacheBookmarkMappingsForTest(
        L"bm3", L"docs", numberedResponse);
    Check(numberedSuggestions.size() == 3 &&
        numberedSuggestions[0] == L"Docs" && numberedSuggestions[1] == L"Docs (2)" &&
        numberedSuggestions[2] == L"Docs (2) (2)",
        "generated numbering never shadows a literal numbered bookmark name");
    bool numberedMappingsIndependent = true;
    const wchar_t* numberedSelections[] = {L"Docs", L"Docs (2)", L"Docs (2) (2)"};
    const wchar_t* numberedUrls[] = {L"about:blank#0", L"about:blank#1", L"about:blank#2"};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto selection = mappingServer.ResolveUri(
            std::wstring(L"bhl://bookmark?prefix=bm3&selection=") + numberedSelections[i], mappingError);
        numberedMappingsIndependent = numberedMappingsIndependent && selection &&
            selection->kind == ListaryActionKind::BrowserOpen &&
            selection->browserResult.url == numberedUrls[i];
    }
    Check(numberedMappingsIndependent,
        "each uniquely numbered bookmark resolves back to its own URL");
    // Same input in a different order must still produce unique, independent rows.
    SearchResponse reorderedResponse;
    reorderedResponse.results.push_back(makeDocs(L"Docs (2)", L"about:blank#2"));
    reorderedResponse.results.push_back(makeDocs(L"Docs", L"about:blank#0"));
    reorderedResponse.results.push_back(makeDocs(L"Docs", L"about:blank#1"));
    const auto reorderedSuggestions = mappingServer.CacheBookmarkMappingsForTest(
        L"bm4", L"docs", reorderedResponse);
    Check(reorderedSuggestions.size() == 3 &&
        std::set<std::wstring>(reorderedSuggestions.begin(), reorderedSuggestions.end()).size() == 3,
        "bookmark suggestion texts stay unique regardless of result order");
    bool reorderedMappingsIndependent = true;
    const wchar_t* reorderedPairs[][2] = {
        {L"Docs (2)", L"about:blank#2"}, {L"Docs", L"about:blank#0"}, {L"Docs (3)", L"about:blank#1"}};
    for (const auto& pair : reorderedPairs) {
        const auto selection = mappingServer.ResolveUri(
            std::wstring(L"bhl://bookmark?prefix=bm4&selection=") + pair[0], mappingError);
        reorderedMappingsIndependent = reorderedMappingsIndependent && selection &&
            selection->browserResult.url == pair[1];
    }
    Check(reorderedMappingsIndependent,
        "each reordered bookmark still resolves back to its own URL");

    // A refresh replaces both the raw-query action and the old visible rows,
    // even when a deleted/renamed bookmark leaves no matching results.
    const auto previousRows = mappingServer.CacheBookmarkMappingsForTest(
        L"bm-refresh", L"needle", sameTextResponse);
    Check(previousRows.size() == 1 && mappingServer.ResolveUri(
        L"bhl://bookmark?prefix=bm-refresh&selection=needle", mappingError).has_value(),
        "bookmark refresh regression starts with an actionable result");
    const auto emptyRows = mappingServer.CacheBookmarkMappingsForTest(
        L"BM-REFRESH", L"needle", SearchResponse{});
    Check(emptyRows.empty() && !mappingServer.ResolveUri(
        L"bhl://bookmark?prefix=bm-refresh&selection=needle", mappingError),
        "empty bookmark refresh invalidates the old raw-query action case-insensitively");
    Check(!previousRows.empty() && !mappingServer.ResolveUri(L"bhl://bookmark?prefix=bm-refresh&selection=" +
        previousRows.front(), mappingError),
        "empty bookmark refresh invalidates old rendered bookmark rows");

    mappingServer.CacheBookmarkMappingsForTest(L"bm-refresh", L"old-query", sameTextResponse);
    const auto replacementRows = mappingServer.CacheBookmarkMappingsForTest(
        L"bm-refresh", L"new-query", bookmarkResponse);
    Check(!mappingServer.ResolveUri(
        L"bhl://bookmark?prefix=bm-refresh&selection=old-query", mappingError) &&
        !mappingServer.ResolveUri(
            L"bhl://bookmark?prefix=bm-refresh&selection=example.test", mappingError),
        "nonempty bookmark refresh invalidates the previous query and its rows");
    const auto replacementRaw = mappingServer.ResolveUri(
        L"bhl://bookmark?prefix=bm-refresh&selection=new-query", mappingError);
    const auto replacementRow = replacementRows.empty() ? std::optional<ListaryAction>{} :
        mappingServer.ResolveUri(
            L"bhl://bookmark?prefix=bm-refresh&selection=" + replacementRows.front(), mappingError);
    Check(replacementRaw && replacementRow &&
        replacementRaw->browserResult.url == bookmarkResult.url &&
        replacementRow->browserResult.url == bookmarkResult.url &&
        replacementRow->browserResult.profileDirectory == L"Profile 1",
        "refreshed bookmark rows and raw query retain the new URL and source profile");
    Check(mappingServer.ResolveUri(L"bhl://open?prefix=q&selection=百度", mappingError).has_value() &&
        mappingServer.ResolveUri(L"bhl://bluetooth?prefix=ly&selection=air", mappingError).has_value() &&
        mappingServer.ResolveUri(
            L"bhl://bluetooth?prefix=ly&selection=AirPods%20%20%E2%80%94%20%20%E6%9C%AA%E8%BF%9E%E6%8E%A5",
            mappingError).has_value() &&
        mappingServer.ResolveUri(L"bhl://bookmark?prefix=bm2&selection=example.test", mappingError).has_value(),
        "bookmark refresh preserves browser, Bluetooth and other bookmark prefixes");

    const std::filesystem::path ini = argc > 1 ? argv[1] : L"BrowserHistoryLauncher.ini";
    std::wstring warning;
    AppConfig config = ConfigStore::Load(ini, warning);
    std::wstring error;
    Check(ConfigStore::Validate(config, error), "INI configuration validates");
    const auto discoveryStarted = std::chrono::steady_clock::now();
    const auto discovery = BrowserDiscovery::Scan(config);
    const auto discoveryElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - discoveryStarted).count();
    const auto discoveredCompatible = static_cast<std::size_t>(std::count_if(
        discovery.entries.begin(), discovery.entries.end(),
        [](const BrowserDiscoveryEntry& entry) { return entry.chromiumCompatible; }));
    std::cout << "METRIC registered_browsers=" << discovery.entries.size()
              << " compatible_chromium_browsers=" << discoveredCompatible
              << " system_discovery_ms=" << discoveryElapsed << '\n';
    for (const auto& entry : discovery.entries) {
        std::cout << "DISCOVERY name=" << WideToUtf8(entry.name)
                  << " exe=" << WideToUtf8(entry.executable.wstring())
                  << " compatible=" << (entry.chromiumCompatible ? 1 : 0)
                  << " detail=" << WideToUtf8(entry.detail) << '\n';
    }
    Check(!discovery.entries.empty(), "Windows registered-browser discovery returns entries");
    auto systemDiscoveredConfig = config;
    BrowserDiscovery::MergeCompatible(systemDiscoveredConfig, discovery);
    const auto chromeDiscovery = std::find_if(discovery.entries.begin(), discovery.entries.end(),
        [](const BrowserDiscoveryEntry& entry) {
            return entry.chromiumCompatible && entry.executable.filename() == L"chrome.exe";
        });
    const auto edgeDiscovery = std::find_if(discovery.entries.begin(), discovery.entries.end(),
        [](const BrowserDiscoveryEntry& entry) {
            return entry.chromiumCompatible && entry.executable.filename() == L"msedge.exe";
        });
    Check(chromeDiscovery != discovery.entries.end(), "system discovery validates Chrome history structure");
    Check(edgeDiscovery != discovery.entries.end(), "system discovery validates Edge history structure");
    const auto quarkRegistration = std::find_if(discovery.entries.begin(), discovery.entries.end(),
        [](const BrowserDiscoveryEntry& entry) { return entry.executable.filename() == L"quark.exe"; });
    if (quarkRegistration != discovery.entries.end()) {
        Check(quarkRegistration->chromiumCompatible,
            "system discovery validates registered Quark Chromium history structure");
        Check(std::any_of(systemDiscoveredConfig.browsers.begin(), systemDiscoveredConfig.browsers.end(), [](const BrowserDefinition& browser) {
            return std::any_of(browser.executableCandidates.begin(), browser.executableCandidates.end(),
                [](const std::filesystem::path& path) { return path.filename() == L"quark.exe"; });
        }), "compatible Quark browser is merged into runtime configuration");
    }
    auto duplicate = config;
    if (duplicate.browsers.size() >= 2) {
        duplicate.browsers[1].enabled = true;
        duplicate.browsers[1].prefix = duplicate.browsers[0].prefix;
        Check(!ConfigStore::Validate(duplicate, error), "duplicate prefix is rejected");
    }
    auto bluetoothCollision = config;
    if (!bluetoothCollision.browsers.empty()) {
        bluetoothCollision.browsers.front().enabled = true;
        bluetoothCollision.browsers.front().prefix = bluetoothCollision.bluetooth.keyword;
        Check(!ConfigStore::Validate(bluetoothCollision, error),
            "Bluetooth keyword collision with a browser prefix is rejected");
    }

    auto appCollision = config;
    appCollision.appTermination.keyword = appCollision.bluetooth.keyword;
    Check(!ConfigStore::Validate(appCollision, error), "application keyword cannot collide with Bluetooth");
    appCollision.appTermination.keyword = L"bad key";
    Check(!ConfigStore::Validate(appCollision, error), "invalid application keyword is rejected");
    if (!config.browsers.empty()) {
        appCollision.appTermination.keyword = config.browsers.front().prefix;
        appCollision.browsers.front().enabled = true;
        Check(!ConfigStore::Validate(appCollision, error), "application keyword cannot collide with a browser");
    }

    auto bookmarkCollision = config;
    bookmarkCollision.bookmarks.enabled = true;
    bookmarkCollision.bookmarks.keyword = bookmarkCollision.appTermination.keyword;
    bookmarkCollision.appTermination.enabled = true;
    Check(!ConfigStore::Validate(bookmarkCollision, error), "bookmark keyword cannot collide with end");
    bookmarkCollision.bookmarks.keyword = L"bad key";
    Check(!ConfigStore::Validate(bookmarkCollision, error), "invalid bookmark keyword is rejected");
    if (!config.browsers.empty()) {
        bookmarkCollision.bookmarks.keyword = config.browsers.front().prefix;
        bookmarkCollision.browsers.front().enabled = true;
        Check(!ConfigStore::Validate(bookmarkCollision, error), "bookmark keyword cannot collide with a browser");
    }

    const auto tempRoot = std::filesystem::temp_directory_path() /
        (L"BHL-CoreTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(tempRoot, ignored);
    std::filesystem::create_directories(tempRoot);
    RunAppTerminationTests(Check, tempRoot);
    wchar_t selfPathBuffer[32768]{};
    GetModuleFileNameW(nullptr, selfPathBuffer, 32768);
    const auto fakeListary = tempRoot / L"Listary.exe";
    std::filesystem::copy_file(selfPathBuffer, fakeListary,
        std::filesystem::copy_options::overwrite_existing, ignored);
    std::wstring fakeCommand = BrowserLauncher::QuoteArgument(fakeListary.wstring()) + L" --bhl-test-hold";
    STARTUPINFOW fakeStartup{sizeof(fakeStartup)};
    PROCESS_INFORMATION fakeProcess{};
    const bool fakeStarted = CreateProcessW(fakeListary.c_str(), fakeCommand.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, tempRoot.c_str(), &fakeStartup, &fakeProcess) != FALSE;
    if (fakeStarted) CloseHandle(fakeProcess.hThread);
    Sleep(100);
    bool fakeWasRunning = false;
    std::wstring stopError;
    const bool fakeStopped = fakeStarted &&
        ListaryConfigurator::StopForUpdate(fakeListary, fakeWasRunning, stopError);
    Check(fakeStopped && fakeWasRunning &&
        WaitForSingleObject(fakeProcess.hProcess, 3000) == WAIT_OBJECT_0,
        "Listary update flow automatically stops an idle background process");
    if (fakeStarted) {
        if (!fakeStopped) TerminateProcess(fakeProcess.hProcess, 1);
        CloseHandle(fakeProcess.hProcess);
    }
    std::wstring visibleCommand = BrowserLauncher::QuoteArgument(fakeListary.wstring()) + L" --bhl-test-visible";
    PROCESS_INFORMATION visibleProcess{};
    const bool visibleStarted = CreateProcessW(fakeListary.c_str(), visibleCommand.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, tempRoot.c_str(), &fakeStartup, &visibleProcess) != FALSE;
    if (visibleStarted) CloseHandle(visibleProcess.hThread);
    Sleep(200);
    bool visibleWasRunning = false;
    stopError.clear();
    const bool visibleStopped = visibleStarted &&
        ListaryConfigurator::StopForUpdate(fakeListary, visibleWasRunning, stopError);
    Check(!visibleStopped && visibleWasRunning && !stopError.empty() &&
        WaitForSingleObject(visibleProcess.hProcess, 0) == WAIT_TIMEOUT,
        "visible Listary window is not force-closed and enters retry/cancel flow");
    if (visibleStarted) {
        TerminateProcess(visibleProcess.hProcess, 0);
        WaitForSingleObject(visibleProcess.hProcess, 3000);
        CloseHandle(visibleProcess.hProcess);
    }
    if (chromeDiscovery != discovery.entries.end()) {
        const auto exportedIcon = tempRoot / L"chrome.ico";
        Check(BrowserIcon::ExportIco(chromeDiscovery->executable, exportedIcon, error),
            "browser executable icon exports to ICO");
        const auto iconBytes = ReadFile(exportedIcon);
        Check(iconBytes.size() > 6 && iconBytes[0] == '\0' && iconBytes[1] == '\0' &&
            static_cast<unsigned char>(iconBytes[2]) == 1 && iconBytes[3] == '\0',
            "exported browser icon has a valid ICO header");
    }
    if (quarkRegistration != discovery.entries.end() && quarkRegistration->chromiumCompatible) {
        Check(BrowserIcon::ExportIco(quarkRegistration->executable, tempRoot / L"quark.ico", error),
            "system-discovered Quark icon exports to ICO");
    }
    const auto noHotkeyIni = tempRoot / L"no-hotkey.ini";
    {
        std::ofstream noHotkey(noHotkeyIni, std::ios::binary);
        noHotkey << "[app]\nHotkey=none\n";
    }
    std::wstring noHotkeyWarning;
    const auto noHotkeyConfig = ConfigStore::Load(noHotkeyIni, noHotkeyWarning);
    Check(noHotkeyConfig.hotkeyVirtualKey == 0 && noHotkeyConfig.hotkeyModifiers == 0,
        "Hotkey=none disables the native global hotkey");
    Check(!noHotkeyConfig.appTermination.enabled,
        "legacy INI without applications section does not reserve an existing end keyword");
    Check(!noHotkeyConfig.bookmarks.enabled,
        "legacy INI without bookmarks section does not register a new bm keyword");
    const auto editableIni = tempRoot / L"editable.ini";
    std::filesystem::copy_file(ini, editableIni, std::filesystem::copy_options::overwrite_existing, ignored);
    auto editableConfig = ConfigStore::Load(editableIni, warning);
    auto brave = std::find_if(editableConfig.browsers.begin(), editableConfig.browsers.end(),
        [](const BrowserDefinition& browser) { return browser.id == L"brave"; });
    Check(brave != editableConfig.browsers.end(), "config catalog contains Brave definition");
    if (brave != editableConfig.browsers.end()) {
        brave->enabled = true;
        brave->prefix = L"bb";
        brave->iconPath = noHotkeyIni;
        Check(ConfigStore::SaveBrowserSettings(editableIni, *brave, error),
            "browser settings persist to INI");
        const auto persisted = ConfigStore::Load(editableIni, warning);
        const auto saved = std::find_if(persisted.browsers.begin(), persisted.browsers.end(),
            [](const BrowserDefinition& browser) { return browser.id == L"brave"; });
        Check(saved != persisted.browsers.end() && saved->enabled && saved->prefix == L"bb" &&
            saved->iconPath == noHotkeyIni,
            "browser enabled state, keyword, and icon round-trip");
    }
    auto batchConfig = ConfigStore::Load(editableIni, warning);
    auto batchChrome = std::find_if(batchConfig.browsers.begin(), batchConfig.browsers.end(),
        [](const BrowserDefinition& browser) { return browser.id == L"chrome"; });
    auto batchEdge = std::find_if(batchConfig.browsers.begin(), batchConfig.browsers.end(),
        [](const BrowserDefinition& browser) { return browser.id == L"edge"; });
    if (batchChrome != batchConfig.browsers.end() && batchEdge != batchConfig.browsers.end()) {
        batchChrome->prefix = L"gc";
        batchEdge->prefix = L"me";
        Check(ConfigStore::SaveBrowserSettings(editableIni, {*batchChrome, *batchEdge}, error),
            "multiple browser settings persist in one atomic INI replacement");
        const auto batchPersisted = ConfigStore::Load(editableIni, warning);
        const auto savedChrome = std::find_if(batchPersisted.browsers.begin(), batchPersisted.browsers.end(),
            [](const BrowserDefinition& browser) { return browser.id == L"chrome"; });
        const auto savedEdge = std::find_if(batchPersisted.browsers.begin(), batchPersisted.browsers.end(),
            [](const BrowserDefinition& browser) { return browser.id == L"edge"; });
        Check(savedChrome != batchPersisted.browsers.end() && savedEdge != batchPersisted.browsers.end() &&
            savedChrome->prefix == L"gc" && savedEdge->prefix == L"me",
            "batch browser settings round-trip together");
    }
    auto listarySettingsConfig = ConfigStore::Load(editableIni, warning);
    listarySettingsConfig.bluetooth.enabled = false;
    listarySettingsConfig.bluetooth.keyword = L"bt";
    listarySettingsConfig.appTermination.enabled = false;
    listarySettingsConfig.appTermination.keyword = L"quit";
    Check(ConfigStore::SaveListarySettings(editableIni, listarySettingsConfig.browsers,
        listarySettingsConfig.bluetooth, listarySettingsConfig.appTermination, error),
        "browser, Bluetooth and application settings persist atomically");
    const auto listarySettingsPersisted = ConfigStore::Load(editableIni, warning);
    Check(!listarySettingsPersisted.bluetooth.enabled &&
        listarySettingsPersisted.bluetooth.keyword == L"bt",
        "Bluetooth enabled state and keyword round-trip");
    Check(!listarySettingsPersisted.appTermination.enabled && listarySettingsPersisted.appTermination.keyword == L"quit",
        "application enabled state and keyword round-trip");
    Check(ConfigStore::SaveListarySettings(editableIni, listarySettingsConfig.browsers,
        listarySettingsConfig.bluetooth, error) && ConfigStore::Load(editableIni, warning).appTermination.keyword == L"quit",
        "legacy settings API preserves application configuration");
    const auto beforeInvalidSave = ReadFile(editableIni);
    auto invalidAppSettings = listarySettingsConfig.appTermination;
    invalidAppSettings.enabled = true;
    invalidAppSettings.keyword = L"bad key";
    Check(!ConfigStore::SaveListarySettings(editableIni, listarySettingsConfig.browsers,
        listarySettingsConfig.bluetooth, invalidAppSettings, error) && ReadFile(editableIni) == beforeInvalidSave,
        "invalid application settings cannot partially modify INI");
    auto invalidBookmarkSettings = listarySettingsConfig.bookmarks;
    invalidBookmarkSettings.enabled = true;
    invalidBookmarkSettings.keyword = L"bad key";
    Check(!ConfigStore::SaveListarySettings(editableIni, listarySettingsConfig.browsers,
        listarySettingsConfig.bluetooth, listarySettingsConfig.appTermination,
        invalidBookmarkSettings, error) && ReadFile(editableIni) == beforeInvalidSave,
        "invalid bookmark settings cannot partially modify INI");
    Check(ConfigStore::SaveListarySettings(editableIni, listarySettingsConfig.browsers,
        listarySettingsConfig.bluetooth, listarySettingsConfig.appTermination,
        listarySettingsConfig.bookmarks, error),
        "bookmark settings persist with the batch settings API");
    const auto bookmarkPersisted = ConfigStore::Load(editableIni, warning);
    Check(bookmarkPersisted.bookmarks.enabled && bookmarkPersisted.bookmarks.keyword == L"bm",
        "bookmark enabled state and keyword round-trip");
    JsonValue jsonRoundTrip;
    std::wstring jsonError;
    Check(ParseJsonUtf8("{\"text\":\"\\u4e2d\\u6587\",\"array\":[true,false,null,-1.25e2]}",
        jsonRoundTrip, jsonError), "JSON parser accepts Listary-compatible values");
    JsonValue reparsedJson;
    Check(ParseJsonUtf8(SerializeJsonUtf8(jsonRoundTrip), reparsedJson, jsonError) &&
        reparsedJson.Find(L"text") && reparsedJson.Find(L"text")->text() == L"中文",
        "JSON serializer preserves Unicode values");

    const auto listaryRoot = tempRoot / L"Listary";
    const auto listaryPreferences = listaryRoot / L"Preferences.json";
    const auto listaryState = listaryRoot / L"ListaryIntegrationState.ini";
    std::filesystem::create_directories(listaryRoot);
    const std::string listarySample =
        "{\"WebSearch\":{\"Items\":{\"Deletions\":[],\"Moves\":[],\"Insertions\":["
        "{\"Index\":-1,\"Info\":null,\"Item\":{\"Keyword\":\"wiki\",\"Url\":\"https://example.test/?q={query}\","
        "\"Title\":\"Wiki\",\"Icon\":{\"Path\":\"\",\"TypeName\":\"Path\"},\"SuggestionProvider\":\"None\",\"SuggestionUrl\":\"\"}}"
        "],\"Updates\":[]}}}";
    Check(WriteUtf8File(listaryPreferences, listarySample), "create isolated Listary preferences");
    AppConfig listaryConfig;
    BrowserDefinition listaryChrome;
    listaryChrome.id = L"chrome";
    listaryChrome.name = L"Google Chrome";
    listaryChrome.prefix = L"g";
    listaryChrome.executableCandidates = {selfPathBuffer};
    listaryChrome.userDataCandidates = {tempRoot};
    listaryChrome.iconPath = noHotkeyIni;
    BrowserDefinition listaryEdge = listaryChrome;
    listaryEdge.id = L"edge";
    listaryEdge.name = L"Microsoft Edge";
    listaryEdge.prefix = L"e";
    listaryConfig.browsers = {listaryChrome, listaryEdge};
    const auto configuredListary = ListaryConfigurator::Configure(listaryConfig, listaryPreferences, listaryState);
    Check(configuredListary.ok && std::filesystem::exists(configuredListary.backupPath),
        "Listary configuration creates a backup");
    JsonValue configuredDocument;
    Check(ParseJsonUtf8(ReadFile(listaryPreferences), configuredDocument, jsonError) &&
        CountBhlInsertions(configuredDocument) == 5 && CountDisabledGoogleUpdates(configuredDocument) == 1,
        "Listary configuration adds browser, Bluetooth, end and bookmark entries and resolves built-in g conflict");
    Check(BhlIconPath(configuredDocument, L"g") == noHotkeyIni.wstring(),
        "Listary insertion uses the configured icon path");
    const auto configuredAgain = ListaryConfigurator::Configure(listaryConfig, listaryPreferences, listaryState);
    Check(configuredAgain.ok && ParseJsonUtf8(ReadFile(listaryPreferences), configuredDocument, jsonError) &&
        CountBhlInsertions(configuredDocument) == 5,
        "Listary configuration is idempotent");
    auto disabledApplications = listaryConfig;
    disabledApplications.appTermination.enabled = false;
    const auto disabledApplicationsResult = ListaryConfigurator::Configure(disabledApplications, listaryPreferences, listaryState);
    Check(disabledApplicationsResult.ok && ParseJsonUtf8(ReadFile(listaryPreferences), configuredDocument, jsonError) &&
        CountBhlInsertions(configuredDocument) == 4 && ReadFile(listaryPreferences).find("bhl://end?") == std::string::npos,
        "disabling end removes only its managed Listary entry");
    auto renamedApplications = listaryConfig;
    renamedApplications.appTermination.keyword = L"quit";
    const auto renamedApplicationsResult = ListaryConfigurator::Configure(renamedApplications, listaryPreferences, listaryState);
    Check(renamedApplicationsResult.ok && ReadFile(listaryPreferences).find("bhl://end?prefix=quit") != std::string::npos &&
        ReadFile(listaryPreferences).find("bhl://end?prefix=end") == std::string::npos,
        "changing application keyword removes the obsolete entry");
    Check(ListaryConfigurator::Configure(listaryConfig, listaryPreferences, listaryState).ok,
        "application keyword can be restored after rename");
    auto disabledBookmarks = listaryConfig;
    disabledBookmarks.bookmarks.enabled = false;
    const auto disabledBookmarksResult = ListaryConfigurator::Configure(disabledBookmarks, listaryPreferences, listaryState);
    Check(disabledBookmarksResult.ok && ParseJsonUtf8(ReadFile(listaryPreferences), configuredDocument, jsonError) &&
        CountBhlInsertions(configuredDocument) == 4 && ReadFile(listaryPreferences).find("bhl://bookmark?") == std::string::npos,
        "disabling bookmarks removes only its managed Listary entry");
    Check(ListaryConfigurator::Configure(listaryConfig, listaryPreferences, listaryState).ok,
        "bookmark entry can be restored after disable");
    const auto configuredEdgeOnly = ListaryConfigurator::ConfigureBrowser(
        listaryEdge, L"e", listaryPreferences, listaryState);
    Check(configuredEdgeOnly.ok && ParseJsonUtf8(ReadFile(listaryPreferences), configuredDocument, jsonError) &&
        CountBhlInsertions(configuredDocument) == 4 && !BhlIconPath(configuredDocument, L"e").empty() &&
        CountDisabledGoogleUpdates(configuredDocument) == 0,
        "first per-browser configuration migrates old bulk entries to selected browser only");
    const auto configuredChromeAlso = ListaryConfigurator::ConfigureBrowser(
        listaryChrome, L"g", listaryPreferences, listaryState);
    Check(configuredChromeAlso.ok && ParseJsonUtf8(ReadFile(listaryPreferences), configuredDocument, jsonError) &&
        CountBhlInsertions(configuredDocument) == 5 && CountDisabledGoogleUpdates(configuredDocument) == 1,
        "later per-browser configuration preserves previously selected browsers");
    auto disabledEdge = listaryEdge;
    disabledEdge.enabled = false;
    const auto removedEdge = ListaryConfigurator::ConfigureBrowser(
        disabledEdge, L"e", listaryPreferences, listaryState);
    Check(removedEdge.ok && ParseJsonUtf8(ReadFile(listaryPreferences), configuredDocument, jsonError) &&
        CountBhlInsertions(configuredDocument) == 4 && BhlIconPath(configuredDocument, L"e").empty(),
        "per-browser disable removes only the selected browser");
    const auto removedListary = ListaryConfigurator::Remove(listaryPreferences, listaryState);
    Check(removedListary.ok && ParseJsonUtf8(ReadFile(listaryPreferences), configuredDocument, jsonError) &&
        CountBhlInsertions(configuredDocument) == 0 && CountDisabledGoogleUpdates(configuredDocument) == 0,
        "Listary integration removal restores plugin-owned Google state");

    const std::string conflictSample =
        "{\"WebSearch\":{\"Items\":{\"Insertions\":[{\"Item\":{\"Keyword\":\"g\",\"Url\":\"https://other.test/{query}\"}}],\"Updates\":[]}}}";
    Check(WriteUtf8File(listaryPreferences, conflictSample), "create Listary keyword conflict");
    const std::string beforeConflict = ReadFile(listaryPreferences);
    const auto conflictedListary = ListaryConfigurator::Configure(listaryConfig, listaryPreferences, listaryState);
    Check(!conflictedListary.ok && ReadFile(listaryPreferences) == beforeConflict,
        "Listary keyword conflict fails without modifying preferences");
    const std::string appConflictSample =
        "{\"WebSearch\":{\"Items\":{\"Insertions\":[{\"Item\":{\"Keyword\":\"end\",\"Url\":\"https://other.test/{query}\"}}],\"Updates\":[]}}}";
    Check(WriteUtf8File(listaryPreferences, appConflictSample), "create external end keyword conflict");
    const auto conflictingApplications = ListaryConfigurator::Configure(listaryConfig, listaryPreferences, listaryState);
    Check(!conflictingApplications.ok && ReadFile(listaryPreferences) == appConflictSample,
        "end registration never overwrites another custom Listary action");
    const std::string bookmarkConflictSample =
        "{\"WebSearch\":{\"Items\":{\"Insertions\":[{\"Item\":{\"Keyword\":\"bm\",\"Url\":\"https://other.test/{query}\"}}],\"Updates\":[]}}}";
    Check(WriteUtf8File(listaryPreferences, bookmarkConflictSample), "create external bm keyword conflict");
    const auto conflictingBookmarks = ListaryConfigurator::Configure(listaryConfig, listaryPreferences, listaryState);
    Check(!conflictingBookmarks.ok && ReadFile(listaryPreferences) == bookmarkConflictSample,
        "bookmark registration never overwrites another custom Listary action");
    {
        std::ofstream state(tempRoot / L"Local State", std::ios::binary);
        state << "{\"profile\":{\"info_cache\":{\"Default\":{},\"Profile 1\":{}}}}";
    }
    const auto defaultHistory = tempRoot / L"Default" / L"History";
    const auto profileHistory = tempRoot / L"Profile 1" / L"History";
    Check(CreateHistory(defaultHistory, 100000, true), "create isolated 100k history database");
    Check(CreateHistory(profileHistory, 1, true), "create second profile database");
    std::wstring compatibleDetail;
    Check(BrowserDiscovery::IsCompatibleUserDataRoot(tempRoot, compatibleDetail),
        "synthetic Chromium User Data root passes discovery schema validation");

    BrowserDefinition testBrowser;
    testBrowser.id = L"test";
    testBrowser.name = L"Test Chromium";
    testBrowser.prefix = L"t";
    testBrowser.executableCandidates = {selfPathBuffer};
    testBrowser.userDataCandidates = {tempRoot};

    const auto capturePath = tempRoot / L"launch-arguments.txt";
    SetEnvironmentVariableW(L"BHL_TEST_CAPTURE", capturePath.c_str());
    std::wstring launchError;
    const std::wstring specialUrl = L"https://example.test/a b?q=\\\"x\\\"&v=1";
    Check(BrowserLauncher::OpenUrl(testBrowser, L"Profile 1", specialUrl, launchError),
        "CreateProcessW launches configured executable");
    for (int attempt = 0; attempt < 100 && !std::filesystem::exists(capturePath); ++attempt) Sleep(10);
    std::ifstream captured(capturePath, std::ios::binary);
    std::string capturedProfile;
    std::string capturedUrl;
    std::getline(captured, capturedProfile);
    std::getline(captured, capturedUrl);
    captured.close();
    Check(Utf8ToWide(capturedProfile) == L"--profile-directory=Profile 1" &&
          Utf8ToWide(capturedUrl) == specialUrl, "profile and special URL arguments round-trip safely");
    std::filesystem::remove(capturePath, ignored);
    const std::wstring omniboxSearch = L"? 百度";
    Check(BrowserLauncher::OpenUrl(testBrowser, L"", omniboxSearch, launchError),
        "CreateProcessW launches browser omnibox search argument");
    for (int attempt = 0; attempt < 100 && !std::filesystem::exists(capturePath); ++attempt) Sleep(10);
    std::ifstream capturedSearch(capturePath, std::ios::binary);
    std::getline(capturedSearch, capturedProfile);
    std::getline(capturedSearch, capturedUrl);
    capturedSearch.close();
    Check(capturedProfile.empty() && Utf8ToWide(capturedUrl) == omniboxSearch,
        "Unicode omnibox search argument round-trips safely");
    SetEnvironmentVariableW(L"BHL_TEST_CAPTURE", nullptr);
    Sleep(200);

    SnapshotManager snapshots;
    ChromiumHistoryAdapter adapter(snapshots);
    const auto discoveredQuark = std::find_if(systemDiscoveredConfig.browsers.begin(),
        systemDiscoveredConfig.browsers.end(), [](const BrowserDefinition& browser) {
            return std::any_of(browser.executableCandidates.begin(), browser.executableCandidates.end(),
                [](const std::filesystem::path& path) { return path.filename() == L"quark.exe"; });
        });
    if (discoveredQuark != systemDiscoveredConfig.browsers.end()) {
        const auto quarkProfiles = adapter.DiscoverProfiles(*discoveredQuark);
        Check(!quarkProfiles.empty(), "system-discovered Quark profiles are readable by Chromium adapter");
        if (!quarkProfiles.empty()) {
            const auto quarkRecent = adapter.Search(*discoveredQuark, quarkProfiles.front(), L"", 5);
            std::cout << "METRIC quark_recent_results=" << quarkRecent.results.size()
                      << " quark_error=" << WideToUtf8(quarkRecent.error) << '\n';
            Check(quarkRecent.error.empty(),
                "system-discovered Quark history query completes without adapter error");
        }
    }
    const auto profiles = adapter.DiscoverProfiles(testBrowser);
    Check(profiles.size() == 2, "Local State discovers Default and Profile 1");
    auto filteredBrowser = testBrowser;
    filteredBrowser.enabledProfiles = {L"Profile 1"};
    const auto filteredProfiles = adapter.DiscoverProfiles(filteredBrowser);
    Check(filteredProfiles.size() == 1 && filteredProfiles.front().directoryName == L"Profile 1",
        "EnabledProfiles filters discovered profiles");
    auto defaultProfile = std::find_if(profiles.begin(), profiles.end(), [](const auto& profile) {
        return profile.directoryName == L"Default";
    });
    Check(defaultProfile != profiles.end(), "Default profile is present");
    if (defaultProfile != profiles.end()) {
        const auto started = std::chrono::steady_clock::now();
        const auto search = adapter.Search(testBrowser, *defaultProfile, L"needle", 20);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        Check(search.error.empty() && search.results.size() == 1, "100k substring query returns expected row");
        Check(!search.results.empty() && search.results.front().title.find(L"中文") != std::wstring::npos,
            "Unicode title survives SQLite mapping");
        std::cout << "METRIC synthetic_100k_query_us=" << elapsed << '\n';
        Check(elapsed < 100000, "100k query is under 100 ms");
    }

    const auto brokenHistory = tempRoot / L"Broken" / L"History";
    Check(CreateHistory(brokenHistory, 0, false), "create incompatible schema database");
    BrowserProfile broken{testBrowser.id, L"Broken", L"Broken", tempRoot, brokenHistory};
    const auto brokenResult = adapter.Search(testBrowser, broken, L"x", 20);
    Check(brokenResult.schemaIncompatible && brokenResult.results.empty(), "missing required fields fails safely");

    ChromiumBookmarkAdapter bookmarkAdapter;
    const std::string bookmarksJson =
        "{\"checksum\":\"x\",\"version\":1,\"roots\":{"
        "\"bookmark_bar\":{\"children\":["
        "{\"date_added\":\"13388000000000000\",\"name\":\"GitHub 主页\",\"type\":\"url\",\"url\":\"https://github.com/\"},"
        "{\"children\":["
        "{\"date_added\":\"13389000000000001\",\"name\":\"中文 Needle 书签\",\"type\":\"url\",\"url\":\"https://example.test/中文/needle\"},"
        "{\"date_added\":\"13389000000000002\",\"name\":\"needle 前缀站点\",\"type\":\"url\",\"url\":\"https://prefix.example/x\"}"
        "],\"date_added\":\"13387000000000000\",\"name\":\"工作文件夹\",\"type\":\"folder\"}"
        "],\"name\":\"书签栏\",\"type\":\"folder\"},"
        "\"other\":{\"children\":["
        "{\"date_added\":\"13389500000000003\",\"name\":\"Needle\",\"type\":\"url\",\"url\":\"https://other.example/a\"},"
        "{\"date_added\":\"13386500000000004\",\"name\":\"无关名字\",\"type\":\"url\",\"url\":\"https://needle-url.example/y\"}"
        "],\"name\":\"其他书签\",\"type\":\"folder\"}}}";
    Check(WriteUtf8File(tempRoot / L"Default" / L"Bookmarks", bookmarksJson) &&
        WriteUtf8File(tempRoot / L"Profile 1" / L"Bookmarks", bookmarksJson),
        "create synthetic Chromium bookmark files");
    const auto bookmarkProfiles = bookmarkAdapter.DiscoverProfiles(testBrowser);
    Check(bookmarkProfiles.size() == 2, "bookmark adapter discovers the same Local State profiles");
    if (!bookmarkProfiles.empty()) {
        const auto& defaultBookmarkProfile = bookmarkProfiles.front();
        const auto needleSearch = bookmarkAdapter.Search(testBrowser, defaultBookmarkProfile, L"needle", 20);
        Check(needleSearch.error.empty() && needleSearch.results.size() == 4,
            "bookmark search matches title exact/prefix/contains and URL contains");
        const std::vector<std::wstring> expectedOrder = {
            L"Needle", L"needle 前缀站点", L"中文 Needle 书签", L"无关名字"};
        bool orderMatches = needleSearch.results.size() == expectedOrder.size();
        for (std::size_t i = 0; orderMatches && i < expectedOrder.size(); ++i) {
            orderMatches = needleSearch.results[i].title == expectedOrder[i] &&
                needleSearch.results[i].bookmark;
        }
        Check(orderMatches && needleSearch.results.front().lastVisitTime == 13389500000000003LL,
            "bookmark results are ranked by relevance tiers then date added");
        const auto upperSearch = bookmarkAdapter.Search(testBrowser, defaultBookmarkProfile, L"NEEDLE", 20);
        Check(upperSearch.error.empty() && upperSearch.results.size() == 4,
            "bookmark matching is case-insensitive");
        const auto recentBookmarks = bookmarkAdapter.Search(testBrowser, defaultBookmarkProfile, L"", 3);
        Check(recentBookmarks.error.empty() && recentBookmarks.results.size() == 3 &&
            recentBookmarks.results.front().title == L"Needle",
            "empty bookmark query lists most recently added entries");
    }
    Check(WriteUtf8File(tempRoot / L"Broken" / L"Bookmarks", "{ not json"),
        "create malformed bookmark file");
    BrowserProfile brokenBookmarks{testBrowser.id, L"Broken", L"Broken", tempRoot,
        tempRoot / L"Broken" / L"Bookmarks"};
    const auto brokenBookmarkResult = bookmarkAdapter.Search(testBrowser, brokenBookmarks, L"x", 20);
    Check(!brokenBookmarkResult.error.empty() && brokenBookmarkResult.results.empty(),
        "malformed bookmark file fails safely with an error");

    // Resource baseline: a large bookmark store must still answer bounded
    // queries quickly and stay stable across repeated queries.
    {
        std::string large;
        large.reserve(2 * 1024 * 1024);
        large += "{\"roots\":{\"bookmark_bar\":{\"children\":[";
        for (int i = 0; i < 5000; ++i) {
            if (i) large += ',';
            large += "{\"date_added\":\"13" + std::to_string(30000000000000000LL + i) +
                "\",\"name\":\"Site " + std::to_string(i) + " needle\",\"type\":\"url\",\"url\":\"https://s" +
                std::to_string(i) + ".example/x\"}";
        }
        large += "],\"type\":\"folder\"}},\"version\":1}";
        const auto largeProfile = tempRoot / L"Large" / L"Bookmarks";
        std::filesystem::create_directories(largeProfile.parent_path(), ignored);
        Check(WriteUtf8File(largeProfile, large), "create 5k-entry bookmark file");
        BrowserProfile largeBrowserProfile{testBrowser.id, L"Large", L"Large", tempRoot, largeProfile};
        const auto largeStarted = std::chrono::steady_clock::now();
        const auto largeSearch = bookmarkAdapter.Search(testBrowser, largeBrowserProfile, L"needle", 20);
        const auto largeElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - largeStarted).count();
        Check(largeSearch.error.empty() && largeSearch.results.size() == 20,
            "large bookmark store caps results at the limit");
        const auto largeAgain = bookmarkAdapter.Search(testBrowser, largeBrowserProfile, L"needle", 20);
        Check(largeAgain.error.empty() && largeAgain.results.size() == 20,
            "repeated large bookmark queries stay stable");
        std::cout << "METRIC synthetic_5k_bookmarks_query_us=" << largeElapsed << '\n';
    }

    for (const auto& liveBrowser : config.browsers) {
        if (!liveBrowser.enabled || (liveBrowser.id != L"chrome" && liveBrowser.id != L"edge")) continue;
        const std::string browserLabel = WideToUtf8(liveBrowser.name);
        Check(!BrowserLauncher::FindExecutable(liveBrowser).empty(), (browserLabel + " executable discovery").c_str());
        const auto liveProfiles = adapter.DiscoverProfiles(liveBrowser);
        Check(!liveProfiles.empty(), ("live " + browserLabel + " profile discovery").c_str());
        if (!liveProfiles.empty()) {
            const auto started = std::chrono::steady_clock::now();
            const auto live = adapter.Search(liveBrowser, liveProfiles.front(), L"", 20);
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
            if (!live.error.empty()) std::cout << "DIAGNOSTIC live_" << WideToUtf8(liveBrowser.id)
                                               << "_error=" << WideToUtf8(live.error) << '\n';
            Check(live.error.empty() && !live.results.empty(), ("live " + browserLabel + " read-only recent-history query").c_str());
            std::cout << "METRIC live_" << WideToUtf8(liveBrowser.id) << "_recent_query_us=" << elapsed
                      << " results=" << live.results.size() << '\n';
            const auto warmStarted = std::chrono::steady_clock::now();
            const auto warm = adapter.Search(liveBrowser, liveProfiles.front(), L"", 20);
            const auto warmElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - warmStarted).count();
            Check(warm.error.empty() && !warm.results.empty(), ("live " + browserLabel + " same-session snapshot reuse").c_str());
            std::cout << "METRIC live_" << WideToUtf8(liveBrowser.id) << "_warm_query_us=" << warmElapsed
                      << " results=" << warm.results.size() << '\n';
        }
    }

    snapshots.Clear();
    std::filesystem::remove_all(tempRoot, ignored);
    Check(!std::filesystem::exists(tempRoot), "test database cleanup");
    std::cout << "SUMMARY failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
