#pragma once

#include "ProcessHotkeyEnumerator.h"

#include <QString>
#include <cstdint>

namespace ks::process
{
    // 每个目标单独回执；changed 表示后端已接受变更，verified 表示读回也已确认。
    enum class HotkeyClearCode : std::uint8_t
    {
        Cleared,
        Unsupported,
        InvalidTarget,
        StaleTarget,
        Conflict,
        BackendRejected,
        ReadbackFailed,
        Indeterminate
    };

    struct HotkeyClearReceipt
    {
        HotkeyClearCode code = HotkeyClearCode::InvalidTarget;
        bool changed = false;
        bool verified = false;
        QString message;
        QString identityKey;
        unsigned long backendStatus = 0;
        long ntStatus = 0;
    };

    // R0 列表只传结构化身份；后端重新枚举并取得完整可变快照，绝不解析显示列。
    struct RegisteredHotkeyClearTarget
    {
        std::uint64_t hotkeyObject = 0;
        std::uint64_t sessionGlobals = 0;
        std::uint64_t expectedThreadInfo = 0;
        std::uint64_t expectedWindowHandle = 0;
        std::uint64_t expectedSnapshotHash = 0;
        std::uint32_t processId = 0;
        std::uint64_t processCreationTime100ns = 0;
        std::uint32_t threadId = 0;
        std::uint32_t hotkeyId = 0;
        std::uint32_t modifiers = 0;
        std::uint32_t virtualKey = 0;
        bool confirmedByUser = false;
    };

    // 采集 .lnk 时保存真实文件身份；失败的记录仍可显示，但不能清除。
    bool CaptureHotkeyFileIdentity(const QString& path, HotkeyFileIdentity* identity);

    // 仅做纯值准入；会变化的进程、窗口、文件和 R0 快照在执行时再次核验。
    bool CanClearHotkey(const UserModeHotkeyRecord& record, QString* reason = nullptr);
    QString HotkeyClearIdentityKey(const UserModeHotkeyRecord& record);

    // 同步后端，供调用方放到现有异步任务中；不触碰 QWidget，不弹确认框。
    HotkeyClearReceipt ClearHotkey(const UserModeHotkeyRecord& record);
    HotkeyClearReceipt ClearRegisteredHotkey(const RegisteredHotkeyClearTarget& target);
}
