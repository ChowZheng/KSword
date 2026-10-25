#include "HotkeyClearController.h"
#include "../ArkDriverClient/ArkDriverClient.h"

#include <QDir>
#include <QFileInfo>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlguid.h>

#include <algorithm>
#include <array>
#include <string>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

namespace
{
    class ScopedHandle final
    {
    public:
        explicit ScopedHandle(HANDLE handle = nullptr) : m_handle(handle) {}
        ~ScopedHandle() { if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) ::CloseHandle(m_handle); }
        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;
        HANDLE get() const { return m_handle; }
        bool valid() const { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
    private:
        HANDLE m_handle = nullptr;
    };

    template<typename T>
    class ScopedCom final
    {
    public:
        ~ScopedCom() { reset(); }
        ScopedCom(const ScopedCom&) = delete;
        ScopedCom& operator=(const ScopedCom&) = delete;
        ScopedCom() = default;
        T** out() { reset(); return &m_value; }
        T* get() const { return m_value; }
        void reset() { if (m_value) { m_value->Release(); m_value = nullptr; } }
    private:
        T* m_value = nullptr;
    };

    class ScopedComApartment final
    {
    public:
        ScopedComApartment() : m_result(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
        ~ScopedComApartment() { if (SUCCEEDED(m_result)) ::CoUninitialize(); }
        bool ready() const { return SUCCEEDED(m_result); }
    private:
        HRESULT m_result = E_FAIL;
    };

    std::uint64_t fileTimeValue(const FILETIME& value)
    {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
            static_cast<std::uint64_t>(value.dwLowDateTime);
    }

    bool sameFileIdentity(const ks::process::HotkeyFileIdentity& expected,
        const ks::process::HotkeyFileIdentity& actual)
    {
        return expected.valid && actual.valid &&
            expected.volumeSerialNumber == actual.volumeSerialNumber &&
            expected.fileIndex == actual.fileIndex &&
            expected.fileSize == actual.fileSize &&
            expected.lastWriteTime100ns == actual.lastWriteTime100ns;
    }

    // ShellLink::Save可以合法改变长度与mtime；读回只比较卷/文件索引以确认仍是同一文件。
    bool sameStableFileIdentity(const ks::process::HotkeyFileIdentity& expected,
        const ks::process::HotkeyFileIdentity& actual)
    {
        return expected.valid && actual.valid &&
            expected.volumeSerialNumber == actual.volumeSerialNumber &&
            expected.fileIndex == actual.fileIndex;
    }

    QString normalizedPath(const QString& path)
    {
        return QDir::cleanPath(QDir::toNativeSeparators(path.trimmed())).toCaseFolded();
    }

    // 持有进程查询句柄直到单项操作结束，避免 PID 被复用后误触其它实例。
    bool processIdentityMatches(HANDLE processHandle, std::uint64_t expectedCreationTime)
    {
        if (processHandle == nullptr || expectedCreationTime == 0) return false;
        if (::WaitForSingleObject(processHandle, 0) != WAIT_TIMEOUT) return false;
        FILETIME created{}, exited{}, kernel{}, user{};
        return ::GetProcessTimes(processHandle, &created, &exited, &kernel, &user) != FALSE &&
            fileTimeValue(created) == expectedCreationTime;
    }

    ks::process::HotkeyClearReceipt makeReceipt(ks::process::HotkeyClearCode code,
        const QString& key, const QString& detail = {})
    {
        ks::process::HotkeyClearReceipt receipt;
        receipt.code = code;
        receipt.identityKey = key;
        receipt.message = detail;
        return receipt;
    }

    QString win32Failure()
    {
        return QStringLiteral("win32_error=") + QString::number(::GetLastError());
    }

    QString comFailure(HRESULT status)
    {
        return QStringLiteral("hresult=") + QString::number(static_cast<unsigned long>(status), 16);
    }

    bool loadShortcut(const std::wstring& path, DWORD mode,
        ScopedCom<IShellLinkW>& shellLink, ScopedCom<IPersistFile>& persistFile,
        HRESULT* failure)
    {
        HRESULT status = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(shellLink.out()));
        if (SUCCEEDED(status) && shellLink.get() == nullptr) status = E_POINTER;
        if (SUCCEEDED(status)) status = shellLink.get()->QueryInterface(IID_PPV_ARGS(persistFile.out()));
        if (SUCCEEDED(status) && persistFile.get() == nullptr) status = E_NOINTERFACE;
        if (SUCCEEDED(status)) status = persistFile.get()->Load(path.c_str(), mode);
        if (failure) *failure = status;
        return SUCCEEDED(status);
    }

    // 仅比较采集时的稳定身份；内容或目标变更会由调用方另行拒绝。
    bool shortcutIdentityMatches(const QString& path,
        const ks::process::HotkeyFileIdentity& expected)
    {
        ks::process::HotkeyFileIdentity current;
        return ks::process::CaptureHotkeyFileIdentity(path, &current) &&
            sameFileIdentity(expected, current);
    }
}

namespace ks::process
{
    bool CaptureHotkeyFileIdentity(const QString& path, HotkeyFileIdentity* identity)
    {
        if (identity == nullptr) return false;
        *identity = {};
        const QString nativePath = QDir::toNativeSeparators(path.trimmed());
        if (nativePath.isEmpty()) return false;
        const std::wstring widePath = nativePath.toStdWString();
        ScopedHandle handle(::CreateFileW(widePath.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!handle.valid()) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (::GetFileInformationByHandle(handle.get(), &info) == FALSE) return false;
        const std::uint64_t fileIndex =
            (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow;
        if (info.dwVolumeSerialNumber == 0U || fileIndex == 0U) return false;
        identity->valid = true;
        identity->volumeSerialNumber = info.dwVolumeSerialNumber;
        identity->fileIndex = fileIndex;
        identity->fileSize =
            (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
        identity->lastWriteTime100ns = fileTimeValue(info.ftLastWriteTime);
        return true;
    }

    bool CanClearHotkey(const UserModeHotkeyRecord& record, QString* reason)
    {
        const auto reject = [reason](const char* code)
        {
            if (reason) *reason = QString::fromLatin1(code);
            return false;
        };
        if (record.sourceKind == HotkeySourceKind::MenuShortcut ||
            record.sourceKind == HotkeySourceKind::PeAccelerator ||
            record.sourceKind == HotkeySourceKind::Unknown)
            return reject("unsupported_source");
        if (record.processId == 0U || record.processCreationTime100ns == 0U)
            return reject("missing_process_identity");
        if (record.sourceKind == HotkeySourceKind::WindowActivation)
        {
            if (record.windowHandle == 0U || record.threadId == 0U ||
                record.expectedWindowHotkeyValue == 0U)
                return reject("missing_window_identity");
        }
        else if (record.sourceKind == HotkeySourceKind::ShellLink)
        {
            if (record.objectText.trimmed().isEmpty() ||
                record.shortcutTargetPath.trimmed().isEmpty() ||
                record.expectedShortcutHotkeyValue == 0U ||
                !record.shortcutFileIdentity.valid ||
                record.shortcutFileIdentity.volumeSerialNumber == 0U ||
                record.shortcutFileIdentity.fileIndex == 0U ||
                record.shortcutFileIdentity.fileSize == 0U)
                return reject("missing_shortcut_identity");
        }
        if (reason) reason->clear();
        return true;
    }

    QString HotkeyClearIdentityKey(const UserModeHotkeyRecord& record)
    {
        if (record.sourceKind == HotkeySourceKind::WindowActivation)
            return QString::number(static_cast<int>(record.sourceKind)) + QLatin1Char(':') +
                QString::number(record.processId) + QLatin1Char(':') +
                QString::number(record.processCreationTime100ns) + QLatin1Char(':') +
                QString::number(record.windowHandle);
        if (record.sourceKind == HotkeySourceKind::ShellLink && record.shortcutFileIdentity.valid)
            return QString::number(static_cast<int>(record.sourceKind)) + QLatin1Char(':') +
                QString::number(record.shortcutFileIdentity.volumeSerialNumber) + QLatin1Char(':') +
                QString::number(record.shortcutFileIdentity.fileIndex);
        return {};
    }

    HotkeyClearReceipt ClearHotkey(const UserModeHotkeyRecord& record)
    {
        const QString key = HotkeyClearIdentityKey(record);
        QString reason;
        if (!CanClearHotkey(record, &reason))
        {
            const bool unsupported = reason == QLatin1String("unsupported_source");
            return makeReceipt(unsupported ? HotkeyClearCode::Unsupported : HotkeyClearCode::InvalidTarget,
                key, reason);
        }
        ScopedHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE, record.processId));
        if (!process.valid()) return makeReceipt(HotkeyClearCode::StaleTarget, key, win32Failure());
        if (!processIdentityMatches(process.get(), record.processCreationTime100ns))
            return makeReceipt(HotkeyClearCode::StaleTarget, key, QStringLiteral("process_identity_changed"));

        if (record.sourceKind == HotkeySourceKind::WindowActivation)
        {
            HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(record.windowHandle));
            DWORD ownerPid = 0;
            const DWORD ownerTid = ::GetWindowThreadProcessId(window, &ownerPid);
            if (::IsWindow(window) == FALSE || ownerPid != record.processId ||
                ownerTid != record.threadId)
                return makeReceipt(HotkeyClearCode::StaleTarget, key, QStringLiteral("window_identity_changed"));
            DWORD_PTR current = 0;
            if (::SendMessageTimeoutW(window, WM_GETHOTKEY, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &current) == 0)
                return makeReceipt(HotkeyClearCode::Indeterminate, key, win32Failure());
            if (LOWORD(current) != record.expectedWindowHotkeyValue)
                return makeReceipt(HotkeyClearCode::StaleTarget, key, QStringLiteral("window_hotkey_changed"));
            DWORD_PTR result = 0;
            if (::SendMessageTimeoutW(window, WM_SETHOTKEY, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result) == 0)
                return makeReceipt(HotkeyClearCode::Indeterminate, key, win32Failure());
            if (static_cast<LRESULT>(result) <= 0)
                return makeReceipt(HotkeyClearCode::BackendRejected, key, QStringLiteral("window_clear_rejected"));
            HotkeyClearReceipt receipt = makeReceipt(HotkeyClearCode::ReadbackFailed, key,
                QStringLiteral("window_readback_failed"));
            receipt.changed = true;
            DWORD_PTR after = 0;
            if (::SendMessageTimeoutW(window, WM_GETHOTKEY, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &after) != 0 && LOWORD(after) == 0U)
            {
                receipt.code = HotkeyClearCode::Cleared;
                receipt.verified = true;
                receipt.message.clear();
            }
            return receipt;
        }

        const QString shortcutPath = QDir::toNativeSeparators(record.objectText.trimmed());
        if (!shortcutIdentityMatches(shortcutPath, record.shortcutFileIdentity))
            return makeReceipt(HotkeyClearCode::StaleTarget, key, QStringLiteral("shortcut_identity_changed"));
        ScopedComApartment apartment;
        if (!apartment.ready())
            return makeReceipt(HotkeyClearCode::BackendRejected, key, QStringLiteral("com_apartment_unavailable"));
        const std::wstring widePath = shortcutPath.toStdWString();
        ScopedCom<IShellLinkW> shellLink;
        ScopedCom<IPersistFile> persistFile;
        HRESULT status = E_FAIL;
        if (!loadShortcut(widePath, STGM_READWRITE, shellLink, persistFile, &status))
            return makeReceipt(HotkeyClearCode::BackendRejected, key, comFailure(status));
        WORD currentHotkey = 0;
        std::array<wchar_t, MAX_PATH> targetPath{};
        WIN32_FIND_DATAW fileData{};
        status = shellLink.get()->GetHotkey(&currentHotkey);
        if (SUCCEEDED(status))
            status = shellLink.get()->GetPath(targetPath.data(), static_cast<int>(targetPath.size()),
                &fileData, SLGP_UNCPRIORITY);
        if (FAILED(status) || currentHotkey != record.expectedShortcutHotkeyValue ||
            normalizedPath(QString::fromWCharArray(targetPath.data())) !=
                normalizedPath(record.shortcutTargetPath) ||
            !shortcutIdentityMatches(shortcutPath, record.shortcutFileIdentity))
            return makeReceipt(HotkeyClearCode::StaleTarget, key, QStringLiteral("shortcut_content_changed"));
        status = shellLink.get()->SetHotkey(0U);
        if (SUCCEEDED(status)) status = persistFile.get()->Save(widePath.c_str(), TRUE);
        if (FAILED(status)) return makeReceipt(HotkeyClearCode::BackendRejected, key, comFailure(status));
        persistFile.reset();
        shellLink.reset();
        // 仅通知 Shell 重新读取文件；读回只能证明 .lnk 配置，不能证明 Explorer 内存注册已撤销。
        ::SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, widePath.c_str(), nullptr);
        HotkeyClearReceipt receipt = makeReceipt(HotkeyClearCode::ReadbackFailed, key,
            QStringLiteral("shortcut_readback_failed"));
        receipt.changed = true;
        HotkeyFileIdentity persistedIdentity;
        if (!CaptureHotkeyFileIdentity(shortcutPath, &persistedIdentity) ||
            !sameStableFileIdentity(record.shortcutFileIdentity, persistedIdentity))
        {
            receipt.message = QStringLiteral("shortcut_readback_identity_changed");
            return receipt;
        }
        ScopedCom<IShellLinkW> verifyLink;
        ScopedCom<IPersistFile> verifyPersist;
        if (loadShortcut(widePath, STGM_READ, verifyLink, verifyPersist, &status))
        {
            WORD persistedHotkey = 0;
            std::array<wchar_t, MAX_PATH> persistedTarget{};
            WIN32_FIND_DATAW persistedFileData{};
            status = verifyLink.get()->GetHotkey(&persistedHotkey);
            if (SUCCEEDED(status))
                status = verifyLink.get()->GetPath(persistedTarget.data(),
                    static_cast<int>(persistedTarget.size()), &persistedFileData, SLGP_UNCPRIORITY);
            HotkeyFileIdentity afterReadIdentity;
            if (SUCCEEDED(status) && persistedHotkey == 0U &&
                normalizedPath(QString::fromWCharArray(persistedTarget.data())) ==
                    normalizedPath(record.shortcutTargetPath) &&
                CaptureHotkeyFileIdentity(shortcutPath, &afterReadIdentity) &&
                sameStableFileIdentity(record.shortcutFileIdentity, afterReadIdentity))
            {
                receipt.code = HotkeyClearCode::Cleared;
                receipt.verified = true;
                receipt.message.clear();
            }
        }
        return receipt;
    }
}

namespace
{
    bool completeKeyboardSnapshot(const ksword::ark::KeyboardHotkeyEnumResult& result)
    {
        return result.io.ok && result.version == KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION &&
            result.status == KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK &&
            result.returnedCount == result.totalCount &&
            result.entries.size() == static_cast<std::size_t>(result.returnedCount);
    }

    bool matchesRegisteredTarget(const ksword::ark::KeyboardHotkeyEntry& entry,
        const ks::process::RegisteredHotkeyClearTarget& target)
    {
        return entry.source == KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_HOTKEY_TABLE &&
            entry.hotkeyObject == target.hotkeyObject &&
            entry.processId == target.processId &&
            entry.threadId == target.threadId &&
            entry.hotkeyId == target.hotkeyId &&
            entry.modifiers == target.modifiers &&
            entry.virtualKey == target.virtualKey &&
            (target.sessionGlobals == 0U || entry.sessionGlobals == target.sessionGlobals) &&
            (target.expectedThreadInfo == 0U || entry.threadInfo == target.expectedThreadInfo) &&
            (target.expectedWindowHandle == 0U || entry.windowHandle == target.expectedWindowHandle) &&
            (target.expectedSnapshotHash == 0U || entry.snapshotHash == target.expectedSnapshotHash);
    }

    KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY mutationSnapshot(
        const ksword::ark::KeyboardHotkeyEntry& entry)
    {
        KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY snapshot{};
        snapshot.source = entry.source;
        snapshot.status = entry.status;
        snapshot.flags = entry.flags;
        snapshot.bucketIndex = entry.bucketIndex;
        snapshot.depth = entry.depth;
        snapshot.modifiers = entry.modifiers;
        snapshot.modifierFlags2 = entry.modifierFlags2;
        snapshot.virtualKey = entry.virtualKey;
        snapshot.hotkeyId = entry.hotkeyId;
        snapshot.processId = entry.processId;
        snapshot.threadId = entry.threadId;
        snapshot.lastStatus = entry.lastStatus;
        snapshot.hotkeyObject = entry.hotkeyObject;
        snapshot.nextHotkeyObject = entry.nextHotkeyObject;
        snapshot.sessionGlobals = entry.sessionGlobals;
        snapshot.threadInfo = entry.threadInfo;
        snapshot.threadObject = entry.threadObject;
        snapshot.windowObject = entry.windowObject;
        snapshot.windowHandle = entry.windowHandle;
        snapshot.destinationHandle = entry.destinationHandle;
        snapshot.callbackAddress = entry.callbackAddress;
        snapshot.childListFlink = entry.childListFlink;
        snapshot.childListBlink = entry.childListBlink;
        snapshot.snapshotHash = entry.snapshotHash;
        snapshot.objectSize = entry.objectSize;
        snapshot.entryFlags = entry.entryFlags;
        return snapshot;
    }
}

namespace ks::process
{
    HotkeyClearReceipt ClearRegisteredHotkey(const RegisteredHotkeyClearTarget& target)
    {
        const QString key = QString::number(target.hotkeyObject) + QLatin1Char(':') +
            QString::number(target.processId) + QLatin1Char(':') +
            QString::number(target.processCreationTime100ns);
        if (!target.confirmedByUser)
            return makeReceipt(HotkeyClearCode::InvalidTarget, key, QStringLiteral("confirmation_required"));
        if (target.hotkeyObject == 0U || target.processId == 0U || target.threadId == 0U ||
            target.virtualKey == 0U || target.processCreationTime100ns == 0U)
            return makeReceipt(HotkeyClearCode::InvalidTarget, key, QStringLiteral("missing_registered_identity"));

        ScopedHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE, target.processId));
        if (!process.valid()) return makeReceipt(HotkeyClearCode::StaleTarget, key, win32Failure());
        if (!processIdentityMatches(process.get(), target.processCreationTime100ns))
            return makeReceipt(HotkeyClearCode::StaleTarget, key, QStringLiteral("process_identity_changed"));

        // 驱动 mutation 的 USER 上下文必须由执行线程真实建立，不能伪造回执。
        MSG message{};
        (void)::PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        const unsigned long flags = KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_SYSTEM |
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS |
            KSWORD_ARK_KEYBOARD_ENUM_FLAG_FILTER_PROCESS;
        const ksword::ark::DriverClient client;
        const auto enumeration = client.enumerateKeyboardHotkeys(target.processId, flags, 2048UL);
        if (!completeKeyboardSnapshot(enumeration))
            return makeReceipt(HotkeyClearCode::BackendRejected, key, QStringLiteral("r0_snapshot_incomplete"));

        const ksword::ark::KeyboardHotkeyEntry* selected = nullptr;
        for (const auto& entry : enumeration.entries)
        {
            if (!matchesRegisteredTarget(entry, target)) continue;
            if (selected != nullptr)
                return makeReceipt(HotkeyClearCode::Conflict, key, QStringLiteral("r0_identity_ambiguous"));
            selected = &entry;
        }
        if (selected == nullptr)
            return makeReceipt(HotkeyClearCode::StaleTarget, key, QStringLiteral("r0_target_missing"));
        const unsigned long unsafeFlags = KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_HAS_CHILDREN |
            KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_PLACEHOLDER |
            KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_CALLBACK;
        if ((selected->entryFlags & KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_MUTABLE) == 0U ||
            (selected->entryFlags & KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_MAIN) == 0U ||
            (selected->entryFlags & unsafeFlags) != 0U)
            return makeReceipt(HotkeyClearCode::Unsupported, key, QStringLiteral("r0_target_not_mutable"));

        const auto result = client.mutateKeyboardHotkey(mutationSnapshot(*selected),
            KSWORD_ARK_KEYBOARD_MUTATION_OPERATION_DELETE);
        HotkeyClearReceipt receipt = makeReceipt(HotkeyClearCode::BackendRejected, key,
            result.io.ok ? QStringLiteral("r0_status=") + QString::number(result.response.status)
                         : QString::fromStdString(result.io.message));
        receipt.backendStatus = result.response.status;
        receipt.ntStatus = result.response.lastStatus;
        constexpr unsigned long requiredFlags =
            KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_IDENTITY_VALIDATED |
            KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_CALLER_VALIDATED |
            KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_SNAPSHOT_VALIDATED |
            KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_CHANGED;
        receipt.changed = result.io.ok &&
            (result.response.responseFlags & KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_CHANGED) != 0U &&
            (result.response.responseFlags & KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_ROLLED_BACK) == 0U;
        if (!result.io.ok || result.response.status != KSWORD_ARK_KEYBOARD_MUTATION_STATUS_OK ||
            (result.response.responseFlags & requiredFlags) != requiredFlags)
            return receipt;

        const auto after = client.enumerateKeyboardHotkeys(target.processId, flags, 2048UL);
        if (!completeKeyboardSnapshot(after))
        {
            receipt.code = HotkeyClearCode::ReadbackFailed;
            receipt.message = QStringLiteral("r0_readback_unavailable");
            return receipt;
        }
        for (const auto& entry : after.entries)
        {
            if (entry.hotkeyObject == target.hotkeyObject)
            {
                receipt.code = HotkeyClearCode::ReadbackFailed;
                receipt.message = QStringLiteral("r0_object_still_present");
                return receipt;
            }
        }
        receipt.code = HotkeyClearCode::Cleared;
        receipt.verified = true;
        receipt.message.clear();
        return receipt;
    }
}
