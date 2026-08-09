#include "search_fallback.h"

#include "text_util.h"

#include <algorithm>
#include <cwctype>

bool LooksLikeBrowserAddress(std::wstring_view input) {
    const std::wstring value = Trim(input);
    if (value.empty() || std::any_of(value.begin(), value.end(), [](wchar_t ch) {
            return std::iswspace(ch) != 0;
        })) return false;

    std::wstring_view authority(value);
    const auto path = authority.find_first_of(L"/?#");
    if (path != std::wstring_view::npos) authority = authority.substr(0, path);
    if (StartsWithInsensitive(authority, L"localhost")) return true;
    const auto port = authority.rfind(L':');
    if (port != std::wstring_view::npos && authority.find(L':') == port) {
        const auto portText = authority.substr(port + 1);
        if (!portText.empty() && std::all_of(portText.begin(), portText.end(), [](wchar_t ch) {
                return std::iswdigit(ch) != 0;
            })) authority = authority.substr(0, port);
    }
    if (authority.size() < 3 || authority.front() == L'.' || authority.back() == L'.' ||
        authority.find(L'.') == std::wstring_view::npos) return false;
    return std::all_of(authority.begin(), authority.end(), [](wchar_t ch) {
        return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'-';
    });
}

std::optional<HistoryResult> MakeSearchFallback(const BrowserDefinition& browser,
    std::wstring_view query) {
    const std::wstring normalizedQuery = Trim(query);
    if (normalizedQuery.empty()) return std::nullopt;

    HistoryResult result;
    if (LooksLikeBrowserAddress(normalizedQuery)) {
        result.title = L"使用 " + browser.name + L" 打开 " + normalizedQuery;
        result.url = normalizedQuery;
    } else {
        result.title = L"在 " + browser.name + L" 地址栏搜索“" + normalizedQuery + L"”";
        // Chromium treats a single command-line argument beginning with "? "
        // as an omnibox search using that profile's default provider.
        result.url = L"? " + normalizedQuery;
    }
    result.browserId = browser.id;
    result.browserName = browser.name;
    result.directUrl = true;
    return result;
}
