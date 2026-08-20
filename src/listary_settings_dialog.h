#pragma once

#include "model.h"

#include <Windows.h>

#include <optional>
#include <string_view>
#include <vector>

struct ListarySettingsResult {
    std::vector<BrowserDefinition> browsers;
    BluetoothConfig bluetooth;
};

class ListarySettingsDialog {
public:
    static std::optional<ListarySettingsResult> Show(HINSTANCE instance, HWND owner,
        const AppConfig& config, std::wstring_view discoverySummary = {});
};
