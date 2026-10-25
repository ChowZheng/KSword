#pragma once
// 离线PE指令链完整验证后生成；未匹配身份不得启用热键变更。
#include <ntddk.h>

typedef struct _KSW_HOTKEY_MATCH_PROFILE
{
    ULONG FullTimeDateStamp; // win32kfull.sys PE时间戳。
    ULONG FullImageSize; // win32kfull.sys 映像大小。
    ULONG FullPdbAge; // full RSDS Age。
    GUID FullPdbGuid; // full RSDS GUID。
    ULONG BaseTimeDateStamp; // win32kbase.sys PE时间戳。
    ULONG BaseImageSize; // win32kbase.sys 映像大小。
    ULONG BasePdbAge; // base RSDS Age。
    GUID BasePdbGuid; // base RSDS GUID。
    ULONG TableOffset; // W32GetUserSessionState返回值起的128桶表偏移。
    ULONG SessionCacheOffset; // 同一SessionState的卸载缓存位。
    ULONG RemoveMatchingRva; // HKRemoveMatchingHotkeys函数起始RVA。
    UCHAR RemoveMatchingPrologue[32]; // 对应函数首32字节，运行时再次核验。
} KSW_HOTKEY_MATCH_PROFILE;

static const KSW_HOTKEY_MATCH_PROFILE g_KswHotkeyMatchProfiles[] =
{
    {
        0x5CD0A4AFUL, 0x428000UL, 1UL,
        {0x80DB0813UL, 0x4711U, 0x330DU, {0xD7, 0x06, 0x8D, 0x82, 0x6F, 0xF8, 0xE1, 0xB0}},
        0x6EFDB59EUL, 0x343000UL, 1UL,
        {0x45D0067DUL, 0xCAC4U, 0x0108U, {0x52, 0xE2, 0xB6, 0x43, 0xF0, 0x2D, 0x42, 0xBF}},
        0x3290UL, 0x36A8UL, 0x1C8108UL,
        {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x40, 0x32, 0xED, 0x45}
    }
};
static const ULONG g_KswHotkeyMatchProfileCount = 1UL;
