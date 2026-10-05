// ============================================================
// wpJ6_tests.Review2C.cpp
// 作用：MemoryWorkbenchView 第二轮独立复核给出的补测（第三批，原名 proposed-tests3）。
//       - T20：宿主喂入的进程名与可读写标志必须出现在会话条目标 chip 的文字里；
//       - T21：modulesFailed 必须把状态条"读结果"段改写成通道不可用原因（原断言
//         summaryText 非空是空断言——它恒含"R3 · 进程"）。
//       en-US 下状态条无汉字的断言（原 T22）并入 wpJ6_main.cpp 的 RunI18nSmokeTest，
//       因为 initialize("en-US") 之后进程里不再切回中文，必须排在全部测试之后。
// 入口：RunReview2TestsC（由 wpJ6_main.cpp 调用）。
// ============================================================
#include "wpJ6_common.h"

#include <QAbstractButton>

#include <optional>

namespace wpj6_test
{
    namespace
    {
        // T20（杀 a24/c34）：宿主喂入的进程名与可读写标志必须出现在会话条目标 chip 的文字里。
        void TestTargetChipShowsNameAndReadWrite()
        {
            Harness h;
            h.AttachProcess(6201);
            PumpUntil([]() { return true; }, 10);
            h.view->setAttachedProcessInfoProvider([](std::uint32_t) -> std::optional<ks::ui::AttachedProcessDisplayInfo> {
                ks::ui::AttachedProcessDisplayInfo info;
                info.processName = QStringLiteral("fake.exe");
                info.canReadWrite = true;
                return info;
            });
            PumpFor(60);
            QString chipText;
            for (auto* button : h.view->sessionBarForTest()->findChildren<QAbstractButton*>())
            {
                if (button->text().contains(QStringLiteral("PID")))
                {
                    chipText = button->text();
                }
            }
            WPJ6_CHECK_NOTE(chipText.contains(QStringLiteral("fake.exe")), QStringLiteral("目标 chip 应含进程名，实际：%1").arg(chipText));
            WPJ6_CHECK_NOTE(chipText.contains(QStringLiteral("可读写")), QStringLiteral("目标 chip 应含「可读写」，实际：%1").arg(chipText));
        }

        // T21（杀 a26）：modulesFailed 必须把状态条读结果段改写成通道不可用原因——先塞一个哨兵再看它是否被覆盖。
        void TestModulesFailedOverwritesReadResult()
        {
            Harness h;
            h.AttachProcess(6202);
            PumpUntil([]() { return true; }, 10);
            auto* statusBar = h.view->statusBarForTest();
            statusBar->setReadResultText(QStringLiteral("SENTINEL-读结果"), false);
            WPJ6_CHECK(statusBar->summaryText().contains(QStringLiteral("SENTINEL-读结果")));
            emit h.view->target().modulesFailed(false);
            WPJ6_CHECK_NOTE(
                !statusBar->summaryText().contains(QStringLiteral("SENTINEL-读结果")),
                QStringLiteral("modulesFailed 之后读结果段应被改写，实际：%1").arg(statusBar->summaryText()));
        }
    }

    void RunReview2TestsC()
    {
        TestTargetChipShowsNameAndReadWrite();
        TestModulesFailedOverwritesReadResult();
    }
}
