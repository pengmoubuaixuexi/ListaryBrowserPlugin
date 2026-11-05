#include <WinSock2.h>
#include <WS2tcpip.h>

#include "listary_suggestion_server.h"

#include "app_termination.h"
#include "bluetooth_worker.h"
#include "browser_registry.h"
#include "chromium_bookmark_adapter.h"
#include "chromium_history_adapter.h"
#include "history_search_service.h"
#include "search_fallback.h"
#include "snapshot_manager.h"
#include "text_util.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr std::size_t kMaxRequestBytes = 16 * 1024;
constexpr std::size_t kMaxCachedMappings = 512;

class ClientActivityGuard {
public:
    ClientActivityGuard(std::atomic<unsigned>& inFlight, std::atomic<ULONGLONG>& lastSuggestion)
        : inFlight_(inFlight), lastSuggestion_(lastSuggestion) {
        inFlight_.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ClientActivityGuard() {
        // A slow query gets the full idle grace period after its response too.
        if (suggestion_) lastSuggestion_.store(GetTickCount64(), std::memory_order_release);
        inFlight_.fetch_sub(1, std::memory_order_release);
    }
    ClientActivityGuard(const ClientActivityGuard&) = delete;
    ClientActivityGuard& operator=(const ClientActivityGuard&) = delete;

    void MarkSuggestion() {
        suggestion_ = true;
        lastSuggestion_.store(GetTickCount64(), std::memory_order_release);
    }
private:
    std::atomic<unsigned>& inFlight_;
    std::atomic<ULONGLONG>& lastSuggestion_;
    bool suggestion_ = false;
};

std::string EncodeUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::optional<std::wstring> DecodeUtf8(std::string_view value) {
    if (value.empty()) return std::wstring{};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size) <= 0) return std::nullopt;
    return result;
}

std::optional<std::wstring> DecodeQueryValue(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            decoded.push_back(' ');
            continue;
        }
        if (value[i] != '%') {
            decoded.push_back(value[i]);
            continue;
        }
        if (i + 2 >= value.size()) return std::nullopt;
        unsigned int byte = 0;
        const auto* first = value.data() + i + 1;
        const auto result = std::from_chars(first, first + 2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != first + 2) return std::nullopt;
        decoded.push_back(static_cast<char>(byte));
        i += 2;
    }
    return DecodeUtf8(decoded);
}

std::optional<std::wstring> QueryParameter(std::string_view target, std::string_view name) {
    const auto marker = target.find('?');
    if (marker == std::string_view::npos) return std::nullopt;
    std::string_view query = target.substr(marker + 1);
    while (!query.empty()) {
        const auto separator = query.find('&');
        const auto field = query.substr(0, separator);
        const auto equals = field.find('=');
        if (field.substr(0, equals) == name) {
            return DecodeQueryValue(equals == std::string_view::npos ? std::string_view{} : field.substr(equals + 1));
        }
        if (separator == std::string_view::npos) break;
        query.remove_prefix(separator + 1);
    }
    return std::nullopt;
}

std::string JsonString(std::wstring_view value) {
    std::string result = "\"";
    for (const unsigned char ch : EncodeUtf8(value)) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                result += "\\u00";
                result.push_back(hex[(ch >> 4) & 0x0f]);
                result.push_back(hex[ch & 0x0f]);
            } else {
                result.push_back(static_cast<char>(ch));
            }
        }
    }
    result.push_back('"');
    return result;
}

std::wstring DisplayText(const HistoryResult& item) {
    std::wstring title = Trim(item.title);
    if (title.empty()) title = item.url;
    const auto scheme = item.url.find(L"://");
    if (scheme == std::wstring::npos) return title;
    const auto hostStart = scheme + 3;
    const auto hostEnd = item.url.find_first_of(L"/:?#", hostStart);
    const auto host = item.url.substr(hostStart, hostEnd == std::wstring::npos ? std::wstring::npos : hostEnd - hostStart);
    if (!host.empty() && title.find(host) == std::wstring::npos) title += L"  —  " + host;
    return title;
}

std::wstring AppDisplayComponent(std::wstring_view value, std::size_t limit) {
    std::wstring display = Trim(value);
    for (auto& ch : display) {
        if (ch < L' ') ch = L' ';
    }
    if (display.size() > limit) {
        // Keep a UTF-16 surrogate pair intact at the truncation boundary;
        // otherwise strict UTF-8 encoding would erase the entire suggestion.
        if (limit > 0 && display[limit - 1] >= 0xD800 && display[limit - 1] <= 0xDBFF &&
            display[limit] >= 0xDC00 && display[limit] <= 0xDFFF) --limit;
        display = display.substr(0, limit) + L"…";
    }
    return display;
}

std::wstring AppDisplayText(const AppProcessTarget& target) {
    auto name = AppDisplayComponent(target.displayName, 80);
    if (name.empty()) name = AppDisplayComponent(std::filesystem::path(target.executablePath).filename().wstring(), 80);
    const auto title = AppDisplayComponent(target.windowTitle, 100);
    std::wstring display = L"强制结束 " + name;
    if (!title.empty() && ToLowerInvariant(title) != ToLowerInvariant(name)) display += L"  —  " + title;
    display += L"  —  PID " + std::to_wstring(target.processId);
    return display;
}

std::string SuggestionJson(std::wstring_view query, const SearchResponse& response) {
    std::string json = "[" + JsonString(query) + ",[";
    std::set<std::wstring> emitted;
    bool first = true;
    for (const auto& item : response.results) {
        auto display = DisplayText(item);
        if (display.empty() || !emitted.insert(display).second) continue;
        if (!first) json.push_back(',');
        first = false;
        json += JsonString(display);
    }
    json += "]]";
    return json;
}

std::string SuggestionJson(std::wstring_view query, const std::vector<std::wstring>& suggestions) {
    std::string json = "[" + JsonString(query) + ",[";
    bool first = true;
    for (const auto& suggestion : suggestions) {
        if (!first) json.push_back(',');
        first = false;
        json += JsonString(suggestion);
    }
    json += "]]";
    return json;
}

void SendAll(SOCKET socket, std::string_view data) {
    while (!data.empty()) {
        const int sent = send(socket, data.data(), static_cast<int>(std::min<std::size_t>(data.size(), INT_MAX)), 0);
        if (sent <= 0) return;
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
}

void SendResponse(SOCKET socket, int status, std::string_view reason,
    std::string_view contentType, std::string_view body) {
    std::string response = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n";
    response += "Content-Type: " + std::string(contentType) + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    response.append(body);
    SendAll(socket, response);
}
}

struct ListarySuggestionServer::Impl {
    explicit Impl(const AppConfig& sourceConfig, std::filesystem::path sourceWorkerExecutable)
        : config(sourceConfig), workerExecutable(std::move(sourceWorkerExecutable)), registry(config), adapter(snapshots) {}

    ~Impl() {
        stopping.store(true, std::memory_order_release);
        responseReady.notify_all();
        if (listenSocket != INVALID_SOCKET) {
            shutdown(listenSocket, SD_BOTH);
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
        }
        if (serverThread.joinable()) serverThread.join();
        if (winsockStarted) WSACleanup();
    }

    bool Start(std::wstring& error) {
        WSADATA data{};
        const int startup = WSAStartup(MAKEWORD(2, 2), &data);
        if (startup != 0) {
            error = L"Listary 建议服务无法初始化 Winsock：" + std::to_wstring(startup);
            return false;
        }
        winsockStarted = true;
        listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET) {
            error = L"Listary 建议服务无法创建本地套接字：" + std::to_wstring(WSAGetLastError());
            return false;
        }
        BOOL exclusive = TRUE;
        setsockopt(listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(ListarySuggestionServer::kDefaultPort);
        if (bind(listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
            error = L"Listary 建议服务无法监听 127.0.0.1:" +
                std::to_wstring(ListarySuggestionServer::kDefaultPort) + L"：" + std::to_wstring(WSAGetLastError());
            return false;
        }
        serverThread = std::thread([this] { ServerLoop(); });
        return true;
    }

    std::optional<ListaryAction> ResolveUri(std::wstring_view uri, std::wstring& error) const {
        const auto target = EncodeUtf8(uri);
        if (!StartsWithInsensitive(uri, L"bhl:") || uri.find(L'?') == std::wstring_view::npos) {
            error = L"Listary 返回了无法识别的打开地址。";
            return std::nullopt;
        }
        const auto prefix = QueryParameter(target, "prefix");
        const auto selection = QueryParameter(target, "selection");
        if (!prefix || !selection) {
            error = L"Listary 打开地址缺少功能前缀或结果参数。";
            return std::nullopt;
        }
        const auto key = MappingKey(*prefix, *selection);
        std::lock_guard lock(mappingMutex);
        const auto found = mappings.find(key);
        if (found == mappings.end()) {
            error = L"没有找到对应的 Listary 结果。请重新输入关键字，等待下拉结果出现后再选择。";
            return std::nullopt;
        }
        if (found->second.expiresAt != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() >= found->second.expiresAt) {
            error = L"Listary 结果已经过期。请重新输入关键字并选择最新结果。";
            return std::nullopt;
        }
        return found->second.result;
    }

    void ServerLoop() {
        while (!stopping.load(std::memory_order_acquire)) {
            SOCKET client = accept(listenSocket, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            DWORD timeout = 3000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            HandleClient(client);
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
    }

    void HandleClient(SOCKET client) {
        ClientActivityGuard activity(clientsInFlight, lastSuggestionTick);
        std::string request;
        request.reserve(2048);
        char buffer[2048];
        while (request.size() < kMaxRequestBytes && request.find("\r\n\r\n") == std::string::npos) {
            const int received = recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0) return;
            request.append(buffer, static_cast<std::size_t>(received));
        }
        const auto lineEnd = request.find("\r\n");
        if (lineEnd == std::string::npos) {
            SendResponse(client, 400, "Bad Request", "text/plain; charset=utf-8", "bad request");
            return;
        }
        const std::string_view line(request.data(), lineEnd);
        const auto firstSpace = line.find(' ');
        const auto secondSpace = firstSpace == std::string_view::npos ? std::string_view::npos : line.find(' ', firstSpace + 1);
        if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos || line.substr(0, firstSpace) != "GET") {
            SendResponse(client, 405, "Method Not Allowed", "text/plain; charset=utf-8", "GET only");
            return;
        }
        const auto target = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        const auto pathEnd = target.find('?');
        const auto path = target.substr(0, pathEnd);
        if (path == "/health") {
            SendResponse(client, 200, "OK", "application/json; charset=utf-8", "{\"ok\":true}");
            return;
        }
        if (path != "/suggest") {
            SendResponse(client, 404, "Not Found", "text/plain; charset=utf-8", "not found");
            return;
        }
        activity.MarkSuggestion();
        const auto prefix = QueryParameter(target, "prefix");
        const auto query = QueryParameter(target, "q");
        if (!prefix || !query || query->size() > 2048) {
            SendResponse(client, 400, "Bad Request", "application/json; charset=utf-8", "[\"\",[]]");
            return;
        }
        if (config.bluetooth.enabled &&
            ToLowerInvariant(*prefix) == ToLowerInvariant(config.bluetooth.keyword)) {
            HandleBluetoothSuggestion(client, *prefix, *query);
            return;
        }
        if (config.appTermination.enabled &&
            ToLowerInvariant(*prefix) == ToLowerInvariant(config.appTermination.keyword)) {
            HandleAppSuggestion(client, *prefix, *query);
            return;
        }
        if (config.bookmarks.enabled &&
            ToLowerInvariant(*prefix) == ToLowerInvariant(config.bookmarks.keyword)) {
            HandleBookmarkSuggestion(client, *prefix, *query);
            return;
        }
        const auto* browser = registry.FindByPrefix(*prefix);
        if (!browser) {
            const auto body = SuggestionJson(*query, SearchResponse{});
            SendResponse(client, 200, "OK", "application/json; charset=utf-8", body);
            return;
        }

        auto completed = RunSearch({*browser}, *query, SearchKind::History);
        if (!completed) {
            SendResponse(client, 504, "Gateway Timeout", "application/json; charset=utf-8", "[\"\",[]]");
            return;
        }
        CacheMappings(*browser, *prefix, *query, *completed);
        const auto body = SuggestionJson(*query, *completed);
        SendResponse(client, 200, "OK", "application/json; charset=utf-8", body);
    }

    // Runs a search request on the worker thread and waits for its response.
    // Returns nullopt on timeout; the pending request is cancelled then.
    std::optional<SearchResponse> RunSearch(std::vector<BrowserDefinition> browsers,
        std::wstring_view query, SearchKind kind) {
        const auto requestedGeneration = ++generation;
        if (!searchService) {
            searchService = std::make_unique<HistorySearchService>(adapter, bookmarkAdapter, snapshots,
                [this](SearchResponse&& value) {
                    {
                        std::lock_guard lock(responseMutex);
                        response = std::move(value);
                    }
                    responseReady.notify_one();
                });
        }
        {
            std::lock_guard lock(responseMutex);
            response.reset();
        }
        searchService->Submit(std::move(browsers), std::wstring(query), config.maxResults,
            requestedGeneration, kind);
        std::unique_lock lock(responseMutex);
        const bool ready = responseReady.wait_for(lock, std::chrono::seconds(5), [&] {
            return stopping.load(std::memory_order_acquire) ||
                (response.has_value() && response->generation == requestedGeneration);
        });
        if (!ready || !response) {
            lock.unlock();
            searchService->CancelAndCleanup(++generation);
            return std::nullopt;
        }
        SearchResponse completed = std::move(*response);
        response.reset();
        return completed;
    }

    void HandleBookmarkSuggestion(SOCKET client, std::wstring_view prefix, std::wstring_view query) {
        // An in-flight, failed, or empty refresh must not leave the previous
        // query's raw row or bookmark suggestions executable.
        {
            std::lock_guard lock(mappingMutex);
            RemoveBookmarkMappingsLocked(prefix);
        }
        std::vector<BrowserDefinition> browsers;
        for (const auto& browser : config.browsers) {
            if (browser.enabled) browsers.push_back(browser);
        }
        if (browsers.empty()) {
            SendResponse(client, 200, "OK", "application/json; charset=utf-8",
                SuggestionJson(query, SearchResponse{}));
            return;
        }
        auto completed = RunSearch(std::move(browsers), query, SearchKind::Bookmarks);
        if (!completed) {
            SendResponse(client, 504, "Gateway Timeout", "application/json; charset=utf-8", "[\"\",[]]");
            return;
        }
        const auto suggestions = CacheBookmarkMappings(prefix, query, *completed);
        SendResponse(client, 200, "OK", "application/json; charset=utf-8", SuggestionJson(query, suggestions));
    }

    void HandleBluetoothSuggestion(SOCKET client, std::wstring_view prefix, std::wstring_view query) {
        const auto now = std::chrono::steady_clock::now();
        if (bluetoothCacheInvalidated.exchange(false) || bluetoothCache.empty() || now >= bluetoothCacheExpires) {
            if (workerExecutable.empty()) {
                SendResponse(client, 503, "Service Unavailable", "application/json; charset=utf-8", "[\"\",[]]");
                return;
            }
            auto enumeration = BluetoothWorker::Enumerate(workerExecutable, 5000);
            if (!enumeration.error.empty()) {
                SendResponse(client, 503, "Service Unavailable", "application/json; charset=utf-8", "[\"\",[]]");
                return;
            }
            bluetoothCache = std::move(enumeration.devices);
            bluetoothCacheExpires = now + std::chrono::seconds(config.bluetooth.cacheSeconds);
        }

        const auto normalizedQuery = ToLowerInvariant(Trim(query));
        std::vector<BluetoothDeviceTarget> matches;
        for (const auto& device : bluetoothCache) {
            if (normalizedQuery.empty() || ToLowerInvariant(device.displayName).find(normalizedQuery) != std::wstring::npos) {
                matches.push_back(device);
            }
        }
        std::sort(matches.begin(), matches.end(), [&](const auto& left, const auto& right) {
            if (left.connected != right.connected) return left.connected > right.connected;
            const bool leftPrefix = StartsWithInsensitive(left.displayName, normalizedQuery);
            const bool rightPrefix = StartsWithInsensitive(right.displayName, normalizedQuery);
            if (leftPrefix != rightPrefix) return leftPrefix > rightPrefix;
            return CompareStringOrdinal(left.displayName.c_str(), -1, right.displayName.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
        CacheBluetoothMappings(prefix, query, matches);
        std::vector<std::wstring> suggestions;
        std::map<std::wstring, unsigned> duplicateCounts;
        for (const auto& device : matches) {
            const auto normalizedName = ToLowerInvariant(device.displayName);
            const unsigned duplicate = ++duplicateCounts[normalizedName];
            std::wstring name = device.displayName;
            if (duplicate > 1) name += L" (" + std::to_wstring(duplicate) + L")";
            suggestions.push_back(name + (device.connected ? L"  —  已连接" : L"  —  未连接"));
        }
        SendResponse(client, 200, "OK", "application/json; charset=utf-8", SuggestionJson(query, suggestions));
    }

    void HandleAppSuggestion(SOCKET client, std::wstring_view prefix, std::wstring_view query) {
        // Invalidate the previous selection before enumeration, so an old row
        // cannot launch while the refreshed process list is being collected.
        {
            std::lock_guard lock(mappingMutex);
            RemoveAppMappingsLocked();
        }
        const auto targets = AppTermination::Enumerate(query, config.maxResults);
        const auto suggestions = CacheAppMappings(prefix, query, targets);
        SendResponse(client, 200, "OK", "application/json; charset=utf-8", SuggestionJson(query, suggestions));
    }

    static std::wstring MappingKey(std::wstring_view prefix, std::wstring_view selection) {
        return ToLowerInvariant(prefix) + L"\n" + std::wstring(selection);
    }

    void CacheMappings(const BrowserDefinition& browser, std::wstring_view prefix,
        std::wstring_view query, const SearchResponse& completed) {
        const auto normalizedPrefix = ToLowerInvariant(prefix);
        std::lock_guard lock(mappingMutex);
        for (const auto& result : completed.results) {
            const auto display = DisplayText(result);
            if (!display.empty()) {
                ListaryAction action;
                action.kind = ListaryActionKind::BrowserOpen;
                action.browserResult = result;
                StoreMapping(MappingKey(normalizedPrefix, display), std::move(action));
            }
        }
        // Listary always renders the raw query as its first selectable row. It
        // must mean "search/open this input", independently of the first
        // history suggestion shown below it.
        if (auto fallback = MakeSearchFallback(browser, query)) {
            ListaryAction action;
            action.kind = ListaryActionKind::BrowserOpen;
            action.browserResult = *fallback;
            StoreMapping(MappingKey(normalizedPrefix, query), std::move(action));
        }
    }

    void RemoveBookmarkMappingsLocked(std::wstring_view prefix) {
        const std::wstring keyPrefix = ToLowerInvariant(prefix) + L"\n";
        std::erase_if(mappings, [&](const auto& entry) {
            return entry.first.starts_with(keyPrefix);
        });
        std::erase_if(mappingOrder, [this](const auto& entry) {
            const auto found = mappings.find(entry.first);
            return found == mappings.end() || found->second.sequence != entry.second;
        });
    }

    std::vector<std::wstring> CacheBookmarkMappings(std::wstring_view prefix,
        std::wstring_view query, const SearchResponse& completed) {
        std::lock_guard lock(mappingMutex);
        // Replace the entire visible bookmark list, including with zero rows.
        // Keep other module prefixes and their selection mappings intact.
        RemoveBookmarkMappingsLocked(prefix);
        std::vector<std::wstring> suggestions;
        // Listary resolves a selection purely by its rendered text, and it
        // always renders the raw query as a selectable row. Every bookmark row
        // therefore needs a final display text that is unique and differs from
        // the query, or two rows would share one mapping.
        std::set<std::wstring> used;
        for (const auto& result : completed.results) {
            auto display = DisplayText(result);
            if (display.empty()) continue;
            // Keep the query text reserved for the raw-query row; a bookmark
            // named exactly like the query stays selectable via a marker.
            if (display == query) display += L" · 书签";
            // Numbered suffixes deduplicate both repeated bookmarks and names
            // that already contain a literal "(2)"-style suffix themselves.
            std::wstring candidate = display;
            unsigned attempt = 2;
            while (candidate == query || !used.insert(candidate).second) {
                candidate = display + L" (" + std::to_wstring(attempt++) + L")";
            }
            ListaryAction action;
            action.kind = ListaryActionKind::BrowserOpen;
            action.browserResult = result;
            StoreMapping(MappingKey(prefix, candidate), std::move(action));
            suggestions.push_back(std::move(candidate));
        }
        // The raw-query row opens the top bookmark result, mirroring the
        // Bluetooth module; bm never routes input to an unrelated browser.
        if (!completed.results.empty()) {
            ListaryAction action;
            action.kind = ListaryActionKind::BrowserOpen;
            action.browserResult = completed.results.front();
            StoreMapping(MappingKey(prefix, query), std::move(action));
        }
        return suggestions;
    }

    void CacheBluetoothMappings(std::wstring_view prefix, std::wstring_view query,
        const std::vector<BluetoothDeviceTarget>& devices) {
        std::lock_guard lock(mappingMutex);
        std::map<std::wstring, unsigned> duplicateCounts;
        for (const auto& device : devices) {
            const unsigned duplicate = ++duplicateCounts[ToLowerInvariant(device.displayName)];
            std::wstring name = device.displayName;
            if (duplicate > 1) name += L" (" + std::to_wstring(duplicate) + L")";
            const std::wstring display = name + (device.connected ? L"  —  已连接" : L"  —  未连接");
            ListaryAction action;
            action.kind = ListaryActionKind::BluetoothConnect;
            action.bluetoothTarget = device;
            StoreMapping(MappingKey(prefix, display), std::move(action), std::chrono::seconds(60));
        }
        if (!devices.empty()) {
            ListaryAction action;
            action.kind = ListaryActionKind::BluetoothConnect;
            action.bluetoothTarget = devices.front();
            StoreMapping(MappingKey(prefix, query), std::move(action), std::chrono::seconds(60));
        }
    }

    void RemoveAppMappingsLocked() {
        std::erase_if(mappings, [](const auto& entry) {
            return entry.second.result.kind == ListaryActionKind::AppTerminate;
        });
        std::erase_if(mappingOrder, [this](const auto& entry) {
            const auto found = mappings.find(entry.first);
            return found == mappings.end() || found->second.sequence != entry.second;
        });
    }

    std::vector<std::wstring> CacheAppMappings(std::wstring_view prefix, std::wstring_view query,
        const std::vector<AppProcessTarget>& targets,
        std::chrono::steady_clock::duration ttl = std::chrono::seconds(30)) {
        std::lock_guard lock(mappingMutex);
        RemoveAppMappingsLocked();
        std::vector<std::wstring> suggestions;
        std::set<std::wstring> emitted;
        for (const auto& target : targets) {
            if (suggestions.size() >= config.maxResults) break;
            if (target.processId == 0 || target.creationTime == 0 || target.executablePath.empty()) continue;
            auto display = AppDisplayText(target);
            // Listary's raw-query row must never become a termination shortcut,
            // even when the input exactly matches a rendered suggestion.
            if (display == query || !emitted.insert(display).second) continue;
            ListaryAction action;
            action.kind = ListaryActionKind::AppTerminate;
            action.appTarget = target;
            StoreMapping(MappingKey(prefix, display), std::move(action), ttl);
            suggestions.push_back(std::move(display));
        }
        return suggestions;
    }

    struct CachedMapping {
        ListaryAction result;
        std::uint64_t sequence = 0;
        std::chrono::steady_clock::time_point expiresAt{};
    };

    void StoreMapping(std::wstring key, ListaryAction result,
        std::chrono::steady_clock::duration ttl = std::chrono::steady_clock::duration::zero()) {
        const auto sequence = ++mappingSequence;
        const auto expiresAt = ttl == std::chrono::steady_clock::duration::zero() ?
            std::chrono::steady_clock::time_point{} : std::chrono::steady_clock::now() + ttl;
        mappings.insert_or_assign(key, CachedMapping{std::move(result), sequence, expiresAt});
        mappingOrder.emplace_back(std::move(key), sequence);
        while (mappingOrder.size() > kMaxCachedMappings) {
            auto oldest = std::move(mappingOrder.front());
            mappingOrder.pop_front();
            const auto found = mappings.find(oldest.first);
            if (found != mappings.end() && found->second.sequence == oldest.second) mappings.erase(found);
        }
    }

    AppConfig config;
    std::filesystem::path workerExecutable;
    std::vector<BluetoothDeviceTarget> bluetoothCache;
    std::chrono::steady_clock::time_point bluetoothCacheExpires{};
    BrowserRegistry registry;
    SnapshotManager snapshots;
    ChromiumHistoryAdapter adapter;
    ChromiumBookmarkAdapter bookmarkAdapter;
    std::mutex responseMutex;
    std::condition_variable responseReady;
    std::optional<SearchResponse> response;
    mutable std::mutex mappingMutex;
    std::map<std::wstring, CachedMapping> mappings;
    std::deque<std::pair<std::wstring, std::uint64_t>> mappingOrder;
    std::uint64_t mappingSequence = 0;
    std::atomic<bool> stopping{false};
    std::atomic<bool> bluetoothCacheInvalidated{false};
    std::atomic<unsigned> clientsInFlight{0};
    std::atomic<ULONGLONG> lastSuggestionTick{0};
    SOCKET listenSocket = INVALID_SOCKET;
    std::thread serverThread;
    bool winsockStarted = false;
    std::uint64_t generation = 0;
    std::unique_ptr<HistorySearchService> searchService;
};

ListarySuggestionServer::ListarySuggestionServer(const AppConfig& config,
    std::filesystem::path workerExecutable)
    : impl_(std::make_unique<Impl>(config, std::move(workerExecutable))) {}

ListarySuggestionServer::~ListarySuggestionServer() = default;

bool ListarySuggestionServer::Start(std::wstring& error) {
    return impl_->Start(error);
}

std::optional<ListaryAction> ListarySuggestionServer::ResolveUri(std::wstring_view uri, std::wstring& error) const {
    return impl_->ResolveUri(uri, error);
}

void ListarySuggestionServer::InvalidateBluetoothCache() {
    impl_->bluetoothCacheInvalidated.store(true);
}

bool ListarySuggestionServer::CanRecycleHost(std::chrono::milliseconds idleFor) const {
    if (impl_->clientsInFlight.load(std::memory_order_acquire) != 0) return false;
    const ULONGLONG lastSuggestion = impl_->lastSuggestionTick.load(std::memory_order_acquire);
    const ULONGLONG now = GetTickCount64();
    const auto requiredIdle = static_cast<ULONGLONG>(std::max<std::int64_t>(0, idleFor.count()));
    if (lastSuggestion != 0 && (now < lastSuggestion || now - lastSuggestion < requiredIdle)) return false;
    // Recheck after reading the timestamp in case a request started or finished
    // during the snapshot. This never takes the mapping or response locks.
    return impl_->clientsInFlight.load(std::memory_order_acquire) == 0 &&
        impl_->lastSuggestionTick.load(std::memory_order_acquire) == lastSuggestion;
}

#ifdef BHL_TESTING
void ListarySuggestionServer::CacheMappingsForTest(const BrowserDefinition& browser,
    std::wstring_view prefix, std::wstring_view query, const SearchResponse& response) {
    impl_->CacheMappings(browser, prefix, query, response);
}


void ListarySuggestionServer::CacheBluetoothMappingsForTest(std::wstring_view prefix,
    std::wstring_view query, const std::vector<BluetoothDeviceTarget>& devices) {
    impl_->CacheBluetoothMappings(prefix, query, devices);
}

std::vector<std::wstring> ListarySuggestionServer::CacheBookmarkMappingsForTest(
    std::wstring_view prefix, std::wstring_view query, const SearchResponse& response) {
    return impl_->CacheBookmarkMappings(prefix, query, response);
}

std::vector<std::wstring> ListarySuggestionServer::CacheAppMappingsForTest(std::wstring_view prefix,
    std::wstring_view query, const std::vector<AppProcessTarget>& targets,
    std::chrono::steady_clock::duration ttl) {
    return impl_->CacheAppMappings(prefix, query, targets, ttl);
}
#endif
