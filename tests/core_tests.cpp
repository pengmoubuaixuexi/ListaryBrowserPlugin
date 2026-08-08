#include "browser_launcher.h"
#include "chromium_history_adapter.h"
#include "config_store.h"
#include "query_parser.h"
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
}

int wmain(int argc, wchar_t** argv) {
    wchar_t captureBuffer[32768]{};
    if (GetEnvironmentVariableW(L"BHL_TEST_CAPTURE", captureBuffer, 32768) > 0 && argc >= 3) {
        std::ofstream capture(std::filesystem::path(captureBuffer), std::ios::binary);
        capture << WideToUtf8(argv[1]) << '\n' << WideToUtf8(argv[2]) << '\n';
        return capture ? 0 : 3;
    }

    const auto parsed = ParseQuery(L"G  github");
    Check(!parsed.empty && parsed.prefix == L"g" && parsed.query == L"github", "query parser is case-insensitive");
    Check(IsHttpUrl(L"HTTPS://example.com/a b"), "HTTP URL detection");
    Check(BrowserLauncher::QuoteArgument(L"a b\\\"c") == L"\"a b\\\\\\\"c\"", "CreateProcess argument quoting");

    const std::filesystem::path ini = argc > 1 ? argv[1] : L"BrowserHistoryLauncher.ini";
    std::wstring warning;
    AppConfig config = ConfigStore::Load(ini, warning);
    std::wstring error;
    Check(ConfigStore::Validate(config, error), "INI configuration validates");
    auto duplicate = config;
    if (duplicate.browsers.size() >= 2) {
        duplicate.browsers[1].enabled = true;
        duplicate.browsers[1].prefix = duplicate.browsers[0].prefix;
        Check(!ConfigStore::Validate(duplicate, error), "duplicate prefix is rejected");
    }

    const auto tempRoot = std::filesystem::temp_directory_path() /
        (L"BHL-CoreTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(tempRoot, ignored);
    std::filesystem::create_directories(tempRoot);
    {
        std::ofstream state(tempRoot / L"Local State", std::ios::binary);
        state << "{\"profile\":{\"info_cache\":{\"Default\":{},\"Profile 1\":{}}}}";
    }
    const auto defaultHistory = tempRoot / L"Default" / L"History";
    const auto profileHistory = tempRoot / L"Profile 1" / L"History";
    Check(CreateHistory(defaultHistory, 100000, true), "create isolated 100k history database");
    Check(CreateHistory(profileHistory, 1, true), "create second profile database");

    wchar_t selfPathBuffer[32768]{};
    GetModuleFileNameW(nullptr, selfPathBuffer, 32768);
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
    SetEnvironmentVariableW(L"BHL_TEST_CAPTURE", nullptr);
    Sleep(200);

    SnapshotManager snapshots;
    ChromiumHistoryAdapter adapter(snapshots);
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
