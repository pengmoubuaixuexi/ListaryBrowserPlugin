#pragma once

#include "model.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class ListarySuggestionServer {
public:
    static constexpr std::uint16_t kDefaultPort = 32119;

    explicit ListarySuggestionServer(const AppConfig& config);
    ~ListarySuggestionServer();

    ListarySuggestionServer(const ListarySuggestionServer&) = delete;
    ListarySuggestionServer& operator=(const ListarySuggestionServer&) = delete;

    bool Start(std::wstring& error);
    std::optional<HistoryResult> ResolveUri(std::wstring_view uri, std::wstring& error) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
