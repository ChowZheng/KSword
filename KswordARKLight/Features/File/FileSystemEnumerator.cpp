#include "FileSystemEnumerator.h"

#include "PathNavigator.h"
#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverClient.h"

#include <algorithm>
#include <cwchar>

namespace Ksword::Features::File {
namespace {

// MakeStatusText converts a Win32 error into a compact status string. Input is
// a numeric error code; output is either "OK" or "错误 <code>".
std::wstring MakeStatusText(DWORD errorCode) {
    if (errorCode == ERROR_SUCCESS) {
        return L"OK";
    }
    return L"错误 " + std::to_wstring(errorCode);
}

// FileTimeIsZero checks whether a FILETIME carries no useful value. Input is a
// FILETIME; output is true for all-zero timestamps.
bool FileTimeIsZero(const FILETIME& value) {
    return value.dwLowDateTime == 0 && value.dwHighDateTime == 0;
}

// EntryLess sorts directories before files and compares names case-insensitively.
// Inputs are two row models; output is true when left should appear first.
bool EntryLess(const FileEntry& left, const FileEntry& right) {
    if (left.kind != right.kind) {
        return static_cast<int>(left.kind) < static_cast<int>(right.kind);
    }
    return ::CompareStringOrdinal(left.name.c_str(), -1, right.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
}

} // namespace

DirectoryEnumerationResult FileSystemEnumerator::enumerate(
    const std::wstring& directory, DirectorySource source) const {
    if (directory.empty()) {
        return enumerateDrives();
    }
    if (source == DirectorySource::Driver) { return enumerateDirectoryByDriver(directory); }
    if (source != DirectorySource::Win32) { return enumerateDirectoryByIrp(directory, source); }
    return enumerateDirectory(directory);
}

DirectoryEnumerationResult FileSystemEnumerator::enumerateDirectoryByIrp(
    const std::wstring& directory, DirectorySource source) const {
    DirectoryEnumerationResult result;
    result.directory = directory;
    unsigned long layer = KSWORD_ARK_FILE_IRP_LAYER_RELATED;
    switch (source) {
    case DirectorySource::IrpBaseFs: layer = KSWORD_ARK_FILE_IRP_LAYER_BASE_FS; break;
    case DirectorySource::IrpVpbFs: layer = KSWORD_ARK_FILE_IRP_LAYER_VPB_FS; break;
    case DirectorySource::IrpDevice: layer = KSWORD_ARK_FILE_IRP_LAYER_DEVICE; break;
    default: break;
    }
    std::wstring ntPath = directory;
    for (wchar_t& ch : ntPath) { if (ch == L'/') { ch = L'\\'; } }
    if (ntPath.rfind(L"\\??\\", 0) != 0 && ntPath.rfind(L"\\Device\\", 0) != 0) {
        if (ntPath.rfind(L"\\\\?\\UNC\\", 0) == 0) {
            ntPath = L"\\??\\UNC\\" + ntPath.substr(8);
        } else if (ntPath.rfind(L"\\\\?\\", 0) == 0) {
            ntPath = L"\\??\\" + ntPath.substr(4);
        } else if (ntPath.rfind(L"\\\\", 0) == 0) {
            ntPath = L"\\??\\UNC\\" + ntPath.substr(2);
        } else {
            ntPath = L"\\??\\" + ntPath;
        }
    }
    const ksword::ark::FileIrpDirectoryResult driver =
        ksword::ark::DriverClient().enumerateDirectoryByIrp(ntPath, layer);
    if (!driver.io.ok) {
        result.errorCode = driver.io.win32Error ? driver.io.win32Error : ERROR_GEN_FAILURE;
        result.statusText = driver.unsupported ? L"当前驱动不支持自建 IRP 目录枚举。"
            : L"IRP 目录枚举通信失败：Win32=" + std::to_wstring(result.errorCode);
        return result;
    }
    if (driver.queryStatus != KSWORD_ARK_DIRECTORY_ENUM_STATUS_OK &&
        driver.queryStatus != KSWORD_ARK_DIRECTORY_ENUM_STATUS_PARTIAL) {
        result.errorCode = ERROR_GEN_FAILURE;
        result.statusText = L"IRP 目录枚举失败：状态=" + std::to_wstring(driver.queryStatus) +
            L"，NTSTATUS=" + std::to_wstring(static_cast<unsigned long>(driver.lastStatus));
        return result;
    }
    bool incomplete = driver.capped || driver.queryStatus == KSWORD_ARK_DIRECTORY_ENUM_STATUS_PARTIAL;
    result.entries.reserve(driver.entries.size());
    for (const ksword::ark::DirectoryEntryRecord& sourceEntry : driver.entries) {
        if (sourceEntry.name.empty()) { incomplete = true; continue; }
        if ((sourceEntry.flags & KSWORD_ARK_DIRECTORY_ENTRY_FLAG_NAME_TRUNCATED) != 0U) {
            incomplete = true;
        }
        FileEntry entry;
        entry.name = sourceEntry.name;
        entry.fullPath = PathNavigator::joinChildPath(directory, sourceEntry.name);
        entry.attributes = sourceEntry.fileAttributes;
        entry.kind = (sourceEntry.flags & KSWORD_ARK_DIRECTORY_ENTRY_FLAG_DIRECTORY)
            ? FileEntryKind::Directory : FileEntryKind::File;
        entry.reparsePoint = (sourceEntry.fileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        entry.size = sourceEntry.endOfFile > 0 ? static_cast<ULONGLONG>(sourceEntry.endOfFile) : 0;
        if (sourceEntry.lastWriteTime > 0) {
            const ULONGLONG time = static_cast<ULONGLONG>(sourceEntry.lastWriteTime);
            entry.lastWriteTime.dwLowDateTime = static_cast<DWORD>(time);
            entry.lastWriteTime.dwHighDateTime = static_cast<DWORD>(time >> 32U);
        }
        result.entries.push_back(std::move(entry));
    }
    std::sort(result.entries.begin(), result.entries.end(), EntryLess);
    result.statusText = L"IRP 目录枚举 " + std::to_wstring(result.entries.size()) + L" 项；请求层=" +
        std::to_wstring(driver.requestedLayer) + L"；实际层=" +
        std::to_wstring(driver.resolvedLayer) + L"；接收驱动=" + driver.driverName;
    if (driver.resolvedLayer != driver.requestedLayer) {
        result.statusText += L"（已回退，不能视为所选栈层结果）";
    }
    if (incomplete) { result.statusText += L"（结果不完整）"; }
    return result;
}

DirectoryEnumerationResult FileSystemEnumerator::enumerateDirectoryByDriver(const std::wstring& directory) const {
    DirectoryEnumerationResult result;
    result.directory = directory;
    std::wstring ntPath = directory;
    for (wchar_t& ch : ntPath) {
        if (ch == L'/') { ch = L'\\'; }
    }
    if (ntPath.rfind(L"\\??\\", 0) != 0 && ntPath.rfind(L"\\Device\\", 0) != 0) {
        if (ntPath.rfind(L"\\\\?\\UNC\\", 0) == 0) {
            ntPath = L"\\??\\UNC\\" + ntPath.substr(8);
        } else if (ntPath.rfind(L"\\\\?\\", 0) == 0) {
            ntPath = L"\\??\\" + ntPath.substr(4);
        } else if (ntPath.rfind(L"\\\\", 0) == 0) {
            ntPath = L"\\??\\UNC\\" + ntPath.substr(2);
        } else {
            ntPath = L"\\??\\" + ntPath;
        }
    }
    const ksword::ark::DirectoryEnumerationResult driver =
        ksword::ark::DriverClient().enumerateDirectory(ntPath);
    if (!driver.io.ok) {
        result.errorCode = driver.io.win32Error ? driver.io.win32Error : ERROR_GEN_FAILURE;
        result.statusText = driver.unsupported ? L"R0 目录枚举不可用：请更新驱动。"
            : L"R0 目录枚举通信失败：Win32=" + std::to_wstring(result.errorCode);
        return result;
    }
    if (driver.queryStatus != KSWORD_ARK_DIRECTORY_ENUM_STATUS_OK &&
        driver.queryStatus != KSWORD_ARK_DIRECTORY_ENUM_STATUS_PARTIAL) {
        result.errorCode = ERROR_GEN_FAILURE;
        result.statusText = L"R0 目录枚举失败：状态=" + std::to_wstring(driver.queryStatus) +
            L"，NTSTATUS=" + std::to_wstring(static_cast<unsigned long>(driver.lastStatus));
        return result;
    }
    result.entries.reserve(driver.entries.size());
    bool incomplete = driver.capped || driver.queryStatus == KSWORD_ARK_DIRECTORY_ENUM_STATUS_PARTIAL;
    for (const ksword::ark::DirectoryEntryRecord& source : driver.entries) {
        if (source.name.empty()) { incomplete = true; continue; }
        if ((source.flags & KSWORD_ARK_DIRECTORY_ENTRY_FLAG_NAME_TRUNCATED) != 0U) {
            incomplete = true;
        }
        FileEntry entry;
        entry.name = source.name;
        entry.fullPath = PathNavigator::joinChildPath(directory, source.name);
        entry.attributes = source.fileAttributes;
        entry.kind = (source.flags & KSWORD_ARK_DIRECTORY_ENTRY_FLAG_DIRECTORY)
            ? FileEntryKind::Directory : FileEntryKind::File;
        entry.reparsePoint = (source.fileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        entry.size = source.endOfFile > 0 ? static_cast<ULONGLONG>(source.endOfFile) : 0;
        if (source.lastWriteTime > 0) {
            const ULONGLONG time = static_cast<ULONGLONG>(source.lastWriteTime);
            entry.lastWriteTime.dwLowDateTime = static_cast<DWORD>(time);
            entry.lastWriteTime.dwHighDateTime = static_cast<DWORD>(time >> 32U);
        }
        result.entries.push_back(std::move(entry));
    }
    std::sort(result.entries.begin(), result.entries.end(), EntryLess);
    result.statusText = L"R0 目录枚举完成 " + std::to_wstring(result.entries.size()) + L" 项";
    if (incomplete) {
        result.statusText += L"（结果不完整）";
    }
    return result;
}

std::wstring FileSystemEnumerator::formatAttributes(DWORD attributes) {
    std::wstring text;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        text += L"D";
    }
    if (attributes & FILE_ATTRIBUTE_READONLY) {
        text += L"R";
    }
    if (attributes & FILE_ATTRIBUTE_HIDDEN) {
        text += L"H";
    }
    if (attributes & FILE_ATTRIBUTE_SYSTEM) {
        text += L"S";
    }
    if (attributes & FILE_ATTRIBUTE_ARCHIVE) {
        text += L"A";
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        text += L"L";
    }
    return text.empty() ? L"-" : text;
}

std::wstring FileSystemEnumerator::formatSize(ULONGLONG bytes, FileEntryKind kind) {
    if (kind != FileEntryKind::File) {
        return {};
    }
    return std::to_wstring(bytes);
}

std::wstring FileSystemEnumerator::formatLastWriteTime(const FILETIME& fileTime) {
    if (FileTimeIsZero(fileTime)) {
        return {};
    }
    FILETIME localTime{};
    SYSTEMTIME systemTime{};
    if (!::FileTimeToLocalFileTime(&fileTime, &localTime)) {
        return {};
    }
    if (!::FileTimeToSystemTime(&localTime, &systemTime)) {
        return {};
    }
    wchar_t buffer[32]{};
    if (std::swprintf(buffer, 32, L"%04u-%02u-%02u %02u:%02u:%02u",
            systemTime.wYear,
            systemTime.wMonth,
            systemTime.wDay,
            systemTime.wHour,
            systemTime.wMinute,
            systemTime.wSecond) <= 0) {
        return {};
    }
    return buffer;
}

DirectoryEnumerationResult FileSystemEnumerator::enumerateDrives() const {
    DirectoryEnumerationResult result;
    result.virtualDriveRoot = true;
    result.statusText = L"列出逻辑驱动器";

    const DWORD mask = ::GetLogicalDrives();
    const DWORD bufferChars = ::GetLogicalDriveStringsW(0, nullptr);
    if (mask == 0 && bufferChars == 0) {
        result.errorCode = ::GetLastError();
        result.statusText = MakeStatusText(result.errorCode);
        return result;
    }

    std::vector<wchar_t> buffer(bufferChars + 2, L'\0');
    const DWORD written = ::GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (written == 0 || written >= buffer.size()) {
        result.errorCode = ::GetLastError();
        result.statusText = MakeStatusText(result.errorCode);
        return result;
    }

    for (const wchar_t* drive = buffer.data(); drive && *drive; drive += std::wcslen(drive) + 1) {
        FileEntry entry;
        entry.kind = FileEntryKind::Drive;
        entry.name = drive;
        entry.fullPath = drive;
        entry.attributes = FILE_ATTRIBUTE_DIRECTORY;
        result.entries.push_back(entry);
    }
    result.errorCode = ERROR_SUCCESS;
    result.statusText = L"逻辑驱动器 " + std::to_wstring(result.entries.size()) + L" 项";
    return result;
}

DirectoryEnumerationResult FileSystemEnumerator::enumerateDirectory(const std::wstring& directory) const {
    DirectoryEnumerationResult result;
    result.directory = directory;

    const std::wstring pattern = PathNavigator::makeSearchPattern(directory);
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        result.errorCode = ::GetLastError();
        result.statusText = MakeStatusText(result.errorCode);
        return result;
    }

    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        FileEntry entry;
        entry.name = name;
        entry.fullPath = PathNavigator::joinChildPath(directory, name);
        entry.attributes = data.dwFileAttributes;
        entry.kind = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FileEntryKind::Directory : FileEntryKind::File;
        entry.reparsePoint = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        entry.lastWriteTime = data.ftLastWriteTime;
        entry.size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        result.entries.push_back(entry);
    } while (::FindNextFileW(find, &data));

    const DWORD lastError = ::GetLastError();
    ::FindClose(find);
    if (lastError != ERROR_NO_MORE_FILES) {
        result.errorCode = lastError;
        result.statusText = MakeStatusText(lastError);
        return result;
    }

    std::sort(result.entries.begin(), result.entries.end(), EntryLess);
    result.errorCode = ERROR_SUCCESS;
    result.statusText = L"枚举完成 " + std::to_wstring(result.entries.size()) + L" 项";
    return result;
}

} // namespace Ksword::Features::File
