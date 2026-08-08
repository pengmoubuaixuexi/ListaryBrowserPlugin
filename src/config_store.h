#pragma once

#include "model.h"

#include <filesystem>
#include <string>

class ConfigStore {
public:
    static AppConfig Load(const std::filesystem::path& iniPath, std::wstring& warning);
    static bool Validate(const AppConfig& config, std::wstring& error);
    static AppConfig Defaults();
};
