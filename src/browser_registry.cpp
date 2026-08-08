#include "browser_registry.h"

#include "text_util.h"

const BrowserDefinition* BrowserRegistry::FindByPrefix(std::wstring_view prefix) const {
    const auto normalized = ToLowerInvariant(prefix);
    for (const auto& browser : config_.browsers) {
        if (browser.enabled && ToLowerInvariant(browser.prefix) == normalized) {
            return &browser;
        }
    }
    return nullptr;
}

const BrowserDefinition* BrowserRegistry::FindById(std::wstring_view id) const {
    for (const auto& browser : config_.browsers) {
        if (browser.id == id) {
            return &browser;
        }
    }
    return nullptr;
}
