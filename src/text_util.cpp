#include "text_util.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>

std::wstring Trim(std::wstring_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::iswspace(value[first])) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1])) {
        --last;
    }
    return std::wstring(value.substr(first, last - first));
}

std::wstring ToLowerInvariant(std::wstring_view value) {
    std::wstring result(value);
    if (!result.empty()) {
        CharLowerBuffW(result.data(), static_cast<DWORD>(result.size()));
    }
    return result;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring ExpandEnvironment(std::wstring_view value) {
    std::wstring input(value);
    const DWORD size = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (size == 0) {
        return input;
    }
    std::wstring result(size, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(input.c_str(), result.data(), size);
    if (written == 0 || written > size) {
        return input;
    }
    result.resize(written - 1);
    return result;
}

std::vector<std::wstring> Split(std::wstring_view value, wchar_t delimiter) {
    std::vector<std::wstring> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(delimiter, start);
        const auto part = Trim(value.substr(start, end == std::wstring_view::npos ? value.size() - start : end - start));
        if (!part.empty()) {
            result.push_back(part);
        }
        if (end == std::wstring_view::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

bool StartsWithInsensitive(std::wstring_view value, std::wstring_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    return CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()), prefix.data(),
               static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

std::wstring FormatWindowsError(unsigned long errorCode) {
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errorCode, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = size && buffer ? Trim(std::wstring_view(buffer, size)) : L"Windows 错误 " + std::to_wstring(errorCode);
    if (buffer) {
        LocalFree(buffer);
    }
    return message;
}
