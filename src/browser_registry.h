#pragma once

#include "model.h"

#include <string_view>

class BrowserRegistry {
public:
    explicit BrowserRegistry(const AppConfig& config) : config_(config) {}
    const BrowserDefinition* FindByPrefix(std::wstring_view prefix) const;
    const BrowserDefinition* FindById(std::wstring_view id) const;

private:
    const AppConfig& config_;
};
