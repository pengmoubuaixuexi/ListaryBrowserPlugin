#pragma once

#include "model.h"

#include <Windows.h>

#include <optional>
#include <string_view>

class ListarySettingsDialog {
public:
    static std::optional<BrowserDefinition> Show(HINSTANCE instance, HWND owner,
        const AppConfig& config, std::wstring_view discoverySummary = {});
};
