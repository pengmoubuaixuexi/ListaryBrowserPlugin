#include "query_parser.h"

#include "text_util.h"

#include <cwctype>

ParsedQuery ParseQuery(std::wstring_view input) {
    ParsedQuery result;
    const std::wstring trimmed = Trim(input);
    if (trimmed.empty()) {
        return result;
    }
    result.empty = false;
    std::size_t separator = 0;
    while (separator < trimmed.size() && !std::iswspace(trimmed[separator])) {
        ++separator;
    }
    result.prefix = ToLowerInvariant(std::wstring_view(trimmed).substr(0, separator));
    if (separator < trimmed.size()) {
        result.query = Trim(std::wstring_view(trimmed).substr(separator));
    }
    return result;
}

bool IsHttpUrl(std::wstring_view value) {
    return StartsWithInsensitive(value, L"http://") || StartsWithInsensitive(value, L"https://");
}
