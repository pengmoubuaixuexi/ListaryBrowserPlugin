#pragma once

#include "model.h"

#include <optional>
#include <string>
#include <string_view>

std::wstring EncodeSearchQuery(std::wstring_view query);
std::optional<HistoryResult> MakeSearchFallback(const BrowserDefinition& browser,
    std::wstring_view query);
