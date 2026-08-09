#include "browser_discovery.h"
#include "browser_icon.h"
#include "browser_launcher.h"
#include "chromium_history_adapter.h"
#include "config_store.h"
#include "json_document.h"
#include "listary_configurator.h"
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
        if (url && url->type() == JsonValue::Type::String && StartsWithInsensitive(url->text(), L"bhl://open?")) ++count;
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
    BrowserDefinition genericSearchBrowser;
    genericSearchBrowser.id = L"q";
    genericSearchBrowser.name = L"Quark";
    const auto genericFallback = MakeSearchFallback(genericSearchBrowser, L"百度 a&b");
    Check(genericFallback && genericFallback->url ==
        L"https://www.bing.com/search?q=%E7%99%BE%E5%BA%A6%20a%26b" &&
        genericFallback->title == L"使用 Quark 搜索“百度 a&b”",
        "generic zero-history search fallback URL-encodes Unicode query");
    Check(!MakeSearchFallback(genericSearchBrowser, L""),
        "empty query does not create a web-search fallback");

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

    const auto tempRoot = std::filesystem::temp_directory_path() /
        (L"BHL-CoreTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(tempRoot, ignored);
    std::filesystem::create_directories(tempRoot);
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
        brave->searchUrlTemplate = L"https://search.example.test/?q={query}";
        Check(ConfigStore::SaveBrowserSettings(editableIni, *brave, error),
            "browser settings persist to INI");
        const auto persisted = ConfigStore::Load(editableIni, warning);
        const auto saved = std::find_if(persisted.browsers.begin(), persisted.browsers.end(),
            [](const BrowserDefinition& browser) { return browser.id == L"brave"; });
        Check(saved != persisted.browsers.end() && saved->enabled && saved->prefix == L"bb" &&
            saved->iconPath == noHotkeyIni &&
            saved->searchUrlTemplate == L"https://search.example.test/?q={query}",
            "browser enabled state, keyword, icon, and search URL round-trip");
    }
    wchar_t selfPathBuffer[32768]{};
    GetModuleFileNameW(nullptr, selfPathBuffer, 32768);

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
        CountBhlInsertions(configuredDocument) == 2 && CountDisabledGoogleUpdates(configuredDocument) == 1,
        "Listary configuration adds browser entries and resolves built-in g conflict");
    Check(BhlIconPath(configuredDocument, L"g") == noHotkeyIni.wstring(),
        "Listary insertion uses the configured icon path");
    const auto configuredAgain = ListaryConfigurator::Configure(listaryConfig, listaryPreferences, listaryState);
    Check(configuredAgain.ok && ParseJsonUtf8(ReadFile(listaryPreferences), configuredDocument, jsonError) &&
        CountBhlInsertions(configuredDocument) == 2,
        "Listary configuration is idempotent");
    const auto configuredEdgeOnly = ListaryConfigurator::ConfigureBrowser(
        listaryEdge, L"e", listaryPreferences, listaryState);
    Check(configuredEdgeOnly.ok && ParseJsonUtf8(ReadFile(listaryPreferences), configuredDocument, jsonError) &&
        CountBhlInsertions(configuredDocument) == 1 && !BhlIconPath(configuredDocument, L"e").empty() &&
        CountDisabledGoogleUpdates(configuredDocument) == 0,
        "first per-browser configuration migrates old bulk entries to selected browser only");
    const auto configuredChromeAlso = ListaryConfigurator::ConfigureBrowser(
        listaryChrome, L"g", listaryPreferences, listaryState);
    Check(configuredChromeAlso.ok && ParseJsonUtf8(ReadFile(listaryPreferences), configuredDocument, jsonError) &&
        CountBhlInsertions(configuredDocument) == 2 && CountDisabledGoogleUpdates(configuredDocument) == 1,
        "later per-browser configuration preserves previously selected browsers");
    auto disabledEdge = listaryEdge;
    disabledEdge.enabled = false;
    const auto removedEdge = ListaryConfigurator::ConfigureBrowser(
        disabledEdge, L"e", listaryPreferences, listaryState);
    Check(removedEdge.ok && ParseJsonUtf8(ReadFile(listaryPreferences), configuredDocument, jsonError) &&
        CountBhlInsertions(configuredDocument) == 1 && BhlIconPath(configuredDocument, L"e").empty(),
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
