#pragma once

#include "model.h"

#include <Windows.h>

#include <optional>

class ListarySettingsDialog {
public:
    static std::optional<BrowserDefinition> Show(HINSTANCE instance, HWND owner,
        const AppConfig& config);
};
