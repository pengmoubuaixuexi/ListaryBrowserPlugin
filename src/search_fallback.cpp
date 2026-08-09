#include "search_fallback.h"

#include "query_parser.h"
#include "text_util.h"

#include <iomanip>
#include <sstream>

std::wstring EncodeSearchQuery(std::wstring_view query) {
    const std::string utf8 = WideToUtf8(query);
    std::wostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char ch : utf8) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded << static_cast<wchar_t>(ch);
        } else {
            encoded << L'%' << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(ch);
        }
    }
    return encoded.str();
}

std::optional<HistoryResult> MakeSearchFallback(const BrowserDefinition& browser,
    std::wstring_view query) {
    const std::wstring normalizedQuery = Trim(query);
    if (normalizedQuery.empty() || browser.searchUrlTemplate.empty()) return std::nullopt;
    std::wstring url = browser.searchUrlTemplate;
    const std::wstring encoded = EncodeSearchQuery(normalizedQuery);
    bool replaced = false;
    for (std::size_t position = 0; (position = url.find(L"{query}", position)) != std::wstring::npos;) {
        url.replace(position, 7, encoded);
        position += encoded.size();
        replaced = true;
    }
    if (!replaced || !IsHttpUrl(url)) return std::nullopt;

    HistoryResult result;
    result.title = L"使用 " + browser.name + L" 搜索“" + normalizedQuery + L"”";
    result.url = std::move(url);
    result.browserId = browser.id;
    result.browserName = browser.name;
    result.directUrl = true;
    return result;
}
