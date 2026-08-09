#include "browser_icon.h"

#include "text_util.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace {
#pragma pack(push, 2)
struct GroupIconDirectory {
    WORD reserved;
    WORD type;
    WORD count;
};

struct GroupIconEntry {
    BYTE width;
    BYTE height;
    BYTE colorCount;
    BYTE reserved;
    WORD planes;
    WORD bitCount;
    DWORD bytesInResource;
    WORD resourceId;
};

struct IconDirectoryEntry {
    BYTE width;
    BYTE height;
    BYTE colorCount;
    BYTE reserved;
    WORD planes;
    WORD bitCount;
    DWORD bytesInResource;
    DWORD imageOffset;
};
#pragma pack(pop)

struct ResourceName {
    bool integer = false;
    WORD id = 0;
    std::wstring text;
};

BOOL CALLBACK CollectIconGroup(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR parameter) {
    auto* names = reinterpret_cast<std::vector<ResourceName>*>(parameter);
    if (IS_INTRESOURCE(name)) names->push_back(ResourceName{true, LOWORD(reinterpret_cast<ULONG_PTR>(name)), {}});
    else names->push_back(ResourceName{false, 0, name});
    return TRUE;
}

LPCWSTR ResourcePointer(const ResourceName& name) {
    return name.integer ? MAKEINTRESOURCEW(name.id) : name.text.c_str();
}

bool WriteBytes(std::ofstream& output, const void* data, std::size_t size) {
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(output);
}
}

std::filesystem::path BrowserIcon::CachedIcoPath(std::wstring_view browserId) {
    std::wstring safe;
    for (const wchar_t ch : browserId) {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_') safe.push_back(ch);
    }
    if (safe.empty()) safe = L"browser";
    return std::filesystem::path(ExpandEnvironment(L"%LocalAppData%")) /
        L"BrowserHistoryLauncher" / L"Icons" / (safe + L".ico");
}

bool BrowserIcon::ExportIco(const std::filesystem::path& sourceExecutable,
    const std::filesystem::path& destination, std::wstring& error) {
    HMODULE module = LoadLibraryExW(sourceExecutable.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) {
        error = L"无法读取浏览器图标资源：" + FormatWindowsError(GetLastError());
        return false;
    }

    std::vector<ResourceName> groups;
    EnumResourceNamesW(module, RT_GROUP_ICON, CollectIconGroup,
        reinterpret_cast<LONG_PTR>(&groups));
    if (groups.empty()) {
        FreeLibrary(module);
        error = L"浏览器程序中没有可导出的 ICO 图标资源。";
        return false;
    }

    const HRSRC groupResource = FindResourceW(module, ResourcePointer(groups.front()), RT_GROUP_ICON);
    const HGLOBAL groupHandle = groupResource ? LoadResource(module, groupResource) : nullptr;
    const auto* groupData = groupHandle ? static_cast<const std::byte*>(LockResource(groupHandle)) : nullptr;
    const DWORD groupSize = groupResource ? SizeofResource(module, groupResource) : 0;
    if (!groupData || groupSize < sizeof(GroupIconDirectory)) {
        FreeLibrary(module);
        error = L"浏览器 ICO 目录损坏。";
        return false;
    }

    const auto* directory = reinterpret_cast<const GroupIconDirectory*>(groupData);
    const std::size_t entriesSize = static_cast<std::size_t>(directory->count) * sizeof(GroupIconEntry);
    if (directory->reserved != 0 || directory->type != 1 || directory->count == 0 ||
        sizeof(GroupIconDirectory) + entriesSize > groupSize) {
        FreeLibrary(module);
        error = L"浏览器 ICO 目录格式不受支持。";
        return false;
    }

    const auto* groupEntries = reinterpret_cast<const GroupIconEntry*>(groupData + sizeof(GroupIconDirectory));
    struct Image { IconDirectoryEntry entry{}; const void* data = nullptr; };
    std::vector<Image> images;
    images.reserve(directory->count);
    for (WORD index = 0; index < directory->count; ++index) {
        const HRSRC imageResource = FindResourceW(module, MAKEINTRESOURCEW(groupEntries[index].resourceId), RT_ICON);
        const HGLOBAL imageHandle = imageResource ? LoadResource(module, imageResource) : nullptr;
        const void* imageData = imageHandle ? LockResource(imageHandle) : nullptr;
        const DWORD imageSize = imageResource ? SizeofResource(module, imageResource) : 0;
        if (!imageData || imageSize == 0) continue;
        const auto& source = groupEntries[index];
        images.push_back(Image{IconDirectoryEntry{source.width, source.height, source.colorCount,
            source.reserved, source.planes, source.bitCount, imageSize, 0}, imageData});
    }
    if (images.empty() || images.size() > MAXWORD) {
        FreeLibrary(module);
        error = L"浏览器程序中的图标图像无法读取。";
        return false;
    }
    DWORD offset = static_cast<DWORD>(sizeof(GroupIconDirectory) +
        images.size() * sizeof(IconDirectoryEntry));
    for (auto& image : images) {
        if (offset > MAXDWORD - image.entry.bytesInResource) {
            FreeLibrary(module);
            error = L"浏览器 ICO 文件过大。";
            return false;
        }
        image.entry.imageOffset = offset;
        offset += image.entry.bytesInResource;
    }

    std::error_code fileError;
    std::filesystem::create_directories(destination.parent_path(), fileError);
    if (fileError) {
        FreeLibrary(module);
        error = L"无法创建图标目录：" + Utf8ToWide(fileError.message());
        return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    const GroupIconDirectory outputDirectory{0, 1, static_cast<WORD>(images.size())};
    bool ok = output && WriteBytes(output, &outputDirectory, sizeof(outputDirectory));
    for (const auto& image : images) ok = ok && WriteBytes(output, &image.entry, sizeof(image.entry));
    for (const auto& image : images) ok = ok && WriteBytes(output, image.data, image.entry.bytesInResource);
    output.flush();
    ok = ok && static_cast<bool>(output);
    FreeLibrary(module);
    if (!ok) {
        std::filesystem::remove(destination, fileError);
        error = L"写入浏览器 ICO 文件失败。";
        return false;
    }
    return true;
}
