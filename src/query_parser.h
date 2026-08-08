#pragma once

#include <string>
#include <string_view>

struct ParsedQuery {
    bool empty = true;
    std::wstring prefix;
    std::wstring query;
};

ParsedQuery ParseQuery(std::wstring_view input);
bool IsHttpUrl(std::wstring_view value);
