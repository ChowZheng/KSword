#pragma once

#include "../ProcessDock/HotkeyClearController.h"
#include "../Internationalization/LanguageManager.h"

#include <QString>

namespace ks::window
{
    // R3/R0热键清除共用同一组用户可读回执；后端message仅供诊断，不直接显示内部码。
    inline QString HotkeyClearCodeText(const ks::process::HotkeyClearCode code)
    {
        using Code = ks::process::HotkeyClearCode;
        switch (code)
        {
        case Code::Cleared:
            return ks::i18n::contextText(QStringLiteral("window.global_hotkey.clear.code.cleared"), QStringLiteral("已清除并验证"));
        case Code::Unsupported:
            return ks::i18n::contextText(QStringLiteral("window.global_hotkey.clear.code.unsupported"), QStringLiteral("此来源不支持清除"));
        case Code::InvalidTarget:
            return ks::i18n::contextText(QStringLiteral("window.global_hotkey.clear.code.invalid"), QStringLiteral("目标身份不完整"));
        case Code::StaleTarget:
            return ks::i18n::contextText(QStringLiteral("window.global_hotkey.clear.code.stale"), QStringLiteral("目标已变化"));
        case Code::Conflict:
            return ks::i18n::contextText(QStringLiteral("window.global_hotkey.clear.code.conflict"), QStringLiteral("目标状态冲突"));
        case Code::BackendRejected:
            return ks::i18n::contextText(QStringLiteral("window.global_hotkey.clear.code.rejected"), QStringLiteral("清除请求被拒绝"));
        case Code::ReadbackFailed:
            return ks::i18n::contextText(QStringLiteral("window.global_hotkey.clear.code.readback"), QStringLiteral("更改后读回失败"));
        case Code::Indeterminate:
            return ks::i18n::contextText(QStringLiteral("window.global_hotkey.clear.code.indeterminate"), QStringLiteral("结果未能确认"));
        }
        return ks::i18n::contextText(QStringLiteral("window.global_hotkey.clear.code.unknown"), QStringLiteral("未知结果"));
    }
}
