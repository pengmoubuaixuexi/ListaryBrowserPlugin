#include <WinSock2.h>
#include <WS2tcpip.h>

#include "listary_suggestion_server.h"

#include "browser_registry.h"
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
    explicit Impl(const AppConfig& sourceConfig)
        : config(sourceConfig), registry(config), adapter(snapshots),
          searchService(adapter, snapshots, [this](SearchResponse&& value) {
              {
                  std::lock_guard lock(responseMutex);
                  response = std::move(value);
              }
              responseReady.notify_one();
          }) {}

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

    std::optional<HistoryResult> ResolveUri(std::wstring_view uri, std::wstring& error) const {
        const auto target = EncodeUtf8(uri);
        if (!StartsWithInsensitive(uri, L"bhl:") || uri.find(L'?') == std::wstring_view::npos) {
            error = L"Listary 返回了无法识别的打开地址。";
            return std::nullopt;
        }
        const auto prefix = QueryParameter(target, "prefix");
        const auto selection = QueryParameter(target, "selection");
        if (!prefix || !selection) {
            error = L"Listary 打开地址缺少浏览器或结果参数。";
            return std::nullopt;
        }
        const auto key = MappingKey(*prefix, *selection);
        std::lock_guard lock(mappingMutex);
        const auto found = mappings.find(key);
        if (found == mappings.end()) {
            error = L"没有找到可打开的浏览历史结果。请先确认 Listary 下拉列表中已经出现历史记录，"
                L"再选择其中一条；历史库为空时不能直接回车。";
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
        const auto prefix = QueryParameter(target, "prefix");
        const auto query = QueryParameter(target, "q");
        if (!prefix || !query || query->size() > 2048) {
            SendResponse(client, 400, "Bad Request", "application/json; charset=utf-8", "[\"\",[]]");
            return;
        }
        const auto* browser = registry.FindByPrefix(*prefix);
        if (!browser) {
            const auto body = SuggestionJson(*query, SearchResponse{});
            SendResponse(client, 200, "OK", "application/json; charset=utf-8", body);
            return;
        }

        const auto requestedGeneration = ++generation;
        {
            std::lock_guard lock(responseMutex);
            response.reset();
        }
        searchService.Submit(*browser, *query, config.maxResults, requestedGeneration);
        std::unique_lock lock(responseMutex);
        const bool ready = responseReady.wait_for(lock, std::chrono::seconds(5), [&] {
            return stopping.load(std::memory_order_acquire) ||
                (response.has_value() && response->generation == requestedGeneration);
        });
        if (!ready || !response) {
            lock.unlock();
            searchService.CancelAndCleanup(++generation);
            SendResponse(client, 504, "Gateway Timeout", "application/json; charset=utf-8", "[\"\",[]]");
            return;
        }
        SearchResponse completed = std::move(*response);
        response.reset();
        lock.unlock();
        CacheMappings(*browser, *prefix, *query, completed);
        const auto body = SuggestionJson(*query, completed);
        SendResponse(client, 200, "OK", "application/json; charset=utf-8", body);
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
            if (!display.empty()) StoreMapping(MappingKey(normalizedPrefix, display), result);
        }
        // Listary always renders the raw query as its first selectable row. It
        // must mean "search/open this input", independently of the first
        // history suggestion shown below it.
        if (auto fallback = MakeSearchFallback(browser, query)) {
            StoreMapping(MappingKey(normalizedPrefix, query), *fallback);
        }
    }

    struct CachedMapping {
        HistoryResult result;
        std::uint64_t sequence = 0;
    };

    void StoreMapping(std::wstring key, const HistoryResult& result) {
        const auto sequence = ++mappingSequence;
        mappings.insert_or_assign(key, CachedMapping{result, sequence});
        mappingOrder.emplace_back(std::move(key), sequence);
        while (mappingOrder.size() > kMaxCachedMappings) {
            auto oldest = std::move(mappingOrder.front());
            mappingOrder.pop_front();
            const auto found = mappings.find(oldest.first);
            if (found != mappings.end() && found->second.sequence == oldest.second) mappings.erase(found);
        }
    }

    AppConfig config;
    BrowserRegistry registry;
    SnapshotManager snapshots;
    ChromiumHistoryAdapter adapter;
    std::mutex responseMutex;
    std::condition_variable responseReady;
    std::optional<SearchResponse> response;
    mutable std::mutex mappingMutex;
    std::map<std::wstring, CachedMapping> mappings;
    std::deque<std::pair<std::wstring, std::uint64_t>> mappingOrder;
    std::uint64_t mappingSequence = 0;
    std::atomic<bool> stopping{false};
    SOCKET listenSocket = INVALID_SOCKET;
    std::thread serverThread;
    bool winsockStarted = false;
    std::uint64_t generation = 0;
    HistorySearchService searchService;
};

ListarySuggestionServer::ListarySuggestionServer(const AppConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

ListarySuggestionServer::~ListarySuggestionServer() = default;

bool ListarySuggestionServer::Start(std::wstring& error) {
    return impl_->Start(error);
}

std::optional<HistoryResult> ListarySuggestionServer::ResolveUri(std::wstring_view uri, std::wstring& error) const {
    return impl_->ResolveUri(uri, error);
}

#ifdef BHL_TESTING
void ListarySuggestionServer::CacheMappingsForTest(const BrowserDefinition& browser,
    std::wstring_view prefix, std::wstring_view query, const SearchResponse& response) {
    impl_->CacheMappings(browser, prefix, query, response);
}
#endif
