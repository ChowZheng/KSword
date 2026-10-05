#include "wpG_common.h"

// ============================================================
// wpG_tests.Shots.cpp
// 作用：把会话条 + 状态条拼在一张宿主窗口里，分别在深/浅主题与窄/宽窗口下截图，
// 供人工核对视觉效果（置灰、报红、chip、摘要省略是否正常显示）。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchSessionBar.h"

#include <QDir>
#include <QVBoxLayout>

#include <array>

namespace wpg_test
{
    using ksword::memwb::Channel;
    using ksword::memwb::GateReason;
    using ksword::memwb::GateVerdict;

    namespace
    {
        // BuildHostWidget：装一个"会话条在上、状态条在下"的宿主，贴近真实布局。
        QWidget* BuildHostWidget(ks::ui::WorkbenchSessionBar** sessionBarOut, ks::ui::WorkbenchStatusBar** statusBarOut)
        {
            auto* host = new QWidget();
            auto* layout = new QVBoxLayout(host);

            auto* sessionBar = new ks::ui::WorkbenchSessionBar(host);
            std::array<GateVerdict, 4> verdicts{};
            verdicts[0] = GateVerdict{true, GateReason::None};
            verdicts[1] = GateVerdict{false, GateReason::DriverNotLoaded};
            verdicts[2] = GateVerdict{true, GateReason::ProbeNotDone};
            verdicts[3] = GateVerdict{false, GateReason::SessionNotReady};
            sessionBar->setChannelVerdicts(verdicts);
            sessionBar->setTargetInfo(true, QStringLiteral("chrome.exe"), 4242, 64, true);
            sessionBar->setPendingPatches(96, 3);
            layout->addWidget(sessionBar);

            auto host2 = std::make_unique<FakeDiagnosticsHost>();
            auto* statusBar = new ks::ui::WorkbenchStatusBar(std::move(host2), host);
            statusBar->setChannelScopeText(QStringLiteral("R0 · 进程"));
            statusBar->setReadResultText(QStringLiteral("已读 4096/4096 字节"), false);
            statusBar->setProtection(QStringLiteral("RW"), ks::ui::StatusRole::Success);
            statusBar->setWindowRangeText(QStringLiteral("0x00007FF6_1000..0x00007FF6_2000"));
            statusBar->setWriteResultText(QStringLiteral("已写入 96 字节（3 块）并回读确认。"));
            statusBar->reportScratchAreaDirty(true);
            statusBar->setReadModifyWriteWindow(true);
            statusBar->setDiagnosticsText(
                QStringLiteral("诊断：写入前复核发现目标字节与暂存基线不一致，已拒绝写入。"), false);
            layout->addWidget(statusBar);

            if (sessionBarOut != nullptr) *sessionBarOut = sessionBar;
            if (statusBarOut != nullptr) *statusBarOut = statusBar;
            return host;
        }
    }

    void RunShotsTests(const QString& shotsDir)
    {
        QDir().mkpath(shotsDir);

        for (const bool dark : {false, true})
        {
            ApplyTheme(dark);
            for (const bool wide : {false, true})
            {
                ks::ui::WorkbenchSessionBar* sessionBar = nullptr;
                ks::ui::WorkbenchStatusBar* statusBar = nullptr;
                QWidget* host = BuildHostWidget(&sessionBar, &statusBar);
                host->resize(wide ? 900 : 360, 160);
                host->show();

                const QImage image = GrabImage(*host);
                WPG_CHECK(image.width() > 0 && image.height() > 0);

                const QString fileName = QStringLiteral("%1/wpG_%2_%3.png")
                    .arg(shotsDir)
                    .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                    .arg(wide ? QStringLiteral("wide") : QStringLiteral("narrow"));
                WPG_CHECK_NOTE(image.save(fileName), fileName);

                delete host;
            }
        }
    }
}
