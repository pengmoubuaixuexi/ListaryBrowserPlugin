#pragma once

#include "model.h"

#include <Windows.h>

#include <optional>
#include <string_view>
#include <vector>

class ListarySettingsDialog {
public:
    static std::optional<std::vector<BrowserDefinition>> Show(HINSTANCE instance, HWND owner,
        const AppConfig& config, std::wstring_view discoverySummary = {});
};
