#pragma once

#include "model.h"

#include <optional>
#include <string>
#include <string_view>

bool LooksLikeBrowserAddress(std::wstring_view input);
std::optional<HistoryResult> MakeSearchFallback(const BrowserDefinition& browser,
    std::wstring_view query);
