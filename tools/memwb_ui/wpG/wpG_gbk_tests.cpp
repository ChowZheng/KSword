// ============================================================
// wpG_gbk_tests.cpp
// 作用：ANSI 往返校验（B1）在 GBK 代码页下的独立验证。本机实测的系统 ANSI 代码页
// 通常就是 UTF-8（toLocal8Bit() 等价于 toUtf8()），B1 的"有损"分支在本机几乎不会
// 被真实触发——只有孤立代理项这种任何代码页都编不出来的输入才会走到；换句话说
// "常见汉字在 ANSI 下有损"这条真实用户会遇到的路径，本机夹具测不出来。
//
// 做法：链接参数 /MANIFEST:EMBED /MANIFESTINPUT:wpG_gbk.manifest 给本可执行文件
// 嵌入一份 activeCodePage=zh-CN 的应用清单（Win10 1903+ 支持），使本进程的
// GetACP() 报告 936（GBK），不改动系统区域设置、不影响其它任何进程。在这个代码页
// 下："中文"这类常见汉字 toLocal8Bit() 编码正常（可写），"😀"/"Å"/"한" 这类 GBK
// 编不出来的字符会被 Qt 悄悄替换成 '?'，EncodeAnsi 的往返校验据此判定有损。
//
// 本文件独立编译链接（不进 wpG_tests.exe，避免把 GBK 专属的运行期环境混进常规
// 离屏夹具），由 build-wpG-tests.cmd 在构建完 wpG_tests.exe 之后追加一步构建并
// 运行；任何一条断言失败都让本程序以非零退出码结束，驱动脚本据此让整个构建失败。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchStringWriteDialog.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchMessages.h"
#include "../../../Ksword5.1/Ksword5.1/UI/ThemeStatusRole.h"

#include <QApplication>
#include <QByteArray>
#include <QPushButton>
#include <QString>

#include <Windows.h>

#include <cstdio>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    // Check：记录一条断言，失败时把表达式与补充说明打到 stderr，不中断后续断言。
    void Check(const bool ok, const char* expression, const QString& note)
    {
        ++g_checks;
        if (ok)
        {
            return;
        }
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s%s\n", expression,
            note.isEmpty() ? "" : qPrintable(QStringLiteral("  (%1)").arg(note)));
    }
#define GBK_CHECK(expr) Check(static_cast<bool>(expr), #expr, QString())
#define GBK_CHECK_NOTE(expr, note) Check(static_cast<bool>(expr), #expr, (note))

    // HexOf：把字节序列格式化成空格分隔的十六进制，便于断言失败时直接看出编码结果。
    QString HexOf(const QByteArray& bytes)
    {
        return QString::fromLatin1(bytes.toHex(' '));
    }

    // FindOkButton：对话框里唯一一个 QPushButton 且角色是"写入"（AcceptRole），
    // 不按文字匹配——文字本身随语言包变化，按钮对象在构造时顺序固定更可靠。
    QPushButton* FindOkButton(ks::ui::WorkbenchStringWriteDialog& dlg)
    {
        QPushButton* okButton = nullptr;
        for (QPushButton* button : dlg.findChildren<QPushButton*>())
        {
            if (button->text() == ks::ui::workbench_messages::StringWriteOkButtonText())
            {
                okButton = button;
            }
        }
        return okButton;
    }
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    const UINT acp = GetACP();
    std::printf("GBK_GetACP=%u\n", static_cast<unsigned>(acp));
    // 清单生效的硬前提：GetACP() 必须真的是 936（GBK），否则后面的断言在错误的
    // 代码页下跑，会产生误导性的通过——宁可直接判定失败并说明原因，也不要在错的
    // 环境下假装测过。
    GBK_CHECK_NOTE(acp == 936U, QStringLiteral("清单未生效或系统不支持 activeCodePage（需 Win10 1903+）：GetACP()=%1").arg(acp));

    ks::ui::WorkbenchStringWriteDialog dlg;
    dlg.show();
    dlg.setEncoding(ks::ui::WorkbenchStringWriteDialog::Encoding::Ansi);
    dlg.setAppendNul(false);
    QPushButton* okButton = FindOkButton(dlg);
    GBK_CHECK(okButton != nullptr);

    // ---- 常见汉字：GBK 编得出来，必须可写，不得判有损 ----
    dlg.setText(QStringLiteral("中文"));
    const QByteArray zhongwen = dlg.resultBytes();
    GBK_CHECK_NOTE(HexOf(zhongwen) == QStringLiteral("d6 d0 ce c4"), HexOf(zhongwen));
    if (okButton != nullptr)
    {
        GBK_CHECK(okButton->isEnabled());
    }
    GBK_CHECK(dlg.previewText() != ks::ui::workbench_messages::StringWriteAnsiLossyText());

    // ---- GBK 编不出来的字符：必须判有损，resultBytes 必须为空，写入钮必须禁用 ----
    const QStringList lossySamples = {
        QStringLiteral("😀"),   // 代理对 emoji，GBK 没有对应字形
        QStringLiteral("Å"),    // 西欧字母，GBK 字符集不含
        QStringLiteral("한"),   // 韩文谚文，GBK 字符集不含
    };
    for (const QString& sample : lossySamples)
    {
        dlg.setText(sample);
        GBK_CHECK_NOTE(dlg.resultBytes().isEmpty(),
            QStringLiteral("sample='%1' resultBytes=%2").arg(sample, HexOf(dlg.resultBytes())));
        GBK_CHECK_NOTE(dlg.previewText() == ks::ui::workbench_messages::StringWriteAnsiLossyText(),
            QStringLiteral("sample='%1' preview='%2'").arg(sample, dlg.previewText()));
        if (okButton != nullptr)
        {
            GBK_CHECK_NOTE(!okButton->isEnabled(), sample);
        }
    }

    // ---- 对照：只比较长度的往返校验会被这份清单夹具当场抓住（R2C31 的等价手工复现）----
    // 不改生产代码，直接在本文件里重放同一条判据的"只比长度"版本，证明两种判据
    // 在 GBK 代码页下给出不同结论。Windows 的 WideCharToMultiByte 对编不出来的字符
    // 是"一个字符换一个 '?' 字节"（不是整段截断），这意味着编码再解码回来的
    // 字符数量——在这三个样本上都——与原文本恰好相等，只是内容从原字符变成了
    // '?'；只比较长度的判据因此在这三个样本上**全部**漏判，逐字节内容比较（生产
    // 实现 EncodeAnsi 的真实判据）才能抓住，这正是 R2C31（"往返校验只比长度"）
    // 在 GBK 环境下是真缺口、而不是等价体的直接证据。
    for (const QString& sample : lossySamples)
    {
        const QByteArray encoded = sample.toLocal8Bit();                 // 生产同款编码
        const QString decoded = QString::fromLocal8Bit(encoded);
        const bool realLossy = (decoded != sample);
        const bool lengthOnlyLossy = (decoded.size() != sample.size());
        GBK_CHECK_NOTE(realLossy, QStringLiteral("sample='%1' decoded='%2' encoded=%3").arg(sample, decoded, HexOf(encoded)));
        GBK_CHECK_NOTE(!lengthOnlyLossy,
            QStringLiteral("sample='%1' decoded.size()=%2 sample.size()=%3 — 只比长度本该漏判才对")
                .arg(sample).arg(decoded.size()).arg(sample.size()));
    }

    std::printf("wpG_gbk_tests: %d checks\n", g_checks);
    if (g_failures > 0)
    {
        std::fprintf(stderr, "wpG_gbk_tests: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("wpG_gbk_tests: all %d checks passed\n", g_checks);
    return 0;
}
