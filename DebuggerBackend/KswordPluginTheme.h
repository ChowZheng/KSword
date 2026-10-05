#pragma once
#include <Windows.h>
#include <cstring>

namespace ksword::plugin_theme
{
    // kCopyDataTag 区分自有日志页的主题消息；既有插件窗口握手不变。
    constexpr ULONG_PTR kCopyDataTag = 0x4B535450UL;

    // Packet 是宿主发送的完整颜色快照，COLORREF 不带透明度或进程内指针。
    // version/bytes 约束接收格式，其余字段分别用于根底、编辑底、文字、边框、勾选。
    struct Packet
    {
        DWORD version = 1;
        DWORD bytes = sizeof(Packet);
        COLORREF window = 0;
        COLORREF surface = 0;
        COLORREF text = 0;
        COLORREF border = 0;
        COLORREF accent = 0;
    };
    static_assert(sizeof(Packet) == 28, "Plugin theme packet layout");

    // DecodeCopyData：验证直接父宿主、版本、长度及颜色范围，再复制快照。
    // 输入 parent 为自有日志子窗；输出 packet 仅在完整验证通过后改写。
    inline bool DecodeCopyData(HWND parent, WPARAM sender, LPARAM data, Packet& packet)
    {
        if (sender == 0 || reinterpret_cast<HWND>(sender) != GetParent(parent) || data == 0)
        {
            return false;
        }
        const auto* copyData = reinterpret_cast<const COPYDATASTRUCT*>(data);
        if (copyData->dwData != kCopyDataTag || copyData->cbData != sizeof(Packet) ||
            copyData->lpData == nullptr)
        {
            return false;
        }
        // candidate 使用本地对齐存储，不将跨进程消息缓冲保存到回调以外。
        Packet candidate;
        std::memcpy(&candidate, copyData->lpData, sizeof(candidate));
        if (candidate.version != 1 || candidate.bytes != sizeof(Packet))
        {
            return false;
        }
        // COLORREF 高八位必须为零，避免调色板索引或特殊系统色进入实色画刷。
        const COLORREF combined = candidate.window | candidate.surface | candidate.text |
            candidate.border | candidate.accent;
        if ((combined & 0xFF000000UL) != 0)
        {
            return false;
        }
        packet = candidate;
        return true;
    }
}
