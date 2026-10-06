// wpE_main.cpp
// 作用：WP-E（地址簿）离屏验证夹具的入口。
// 用法：wpE_tests.exe [--shots <目录>]
// 退出码：0 全部通过；1 有断言失败。

#include "wpE_common.h"

#include <QApplication>
#include <QFontDatabase>

#include <iostream>

namespace
{
    QString Arg(int argc, char** argv, const char* name, const QString& defaultValue)
    {
        for (int index = 1; index + 1 < argc; ++index)
        {
            if (QString::fromLocal8Bit(argv[index]) == QString::fromLatin1(name))
            {
                return QString::fromLocal8Bit(argv[index + 1]);
            }
        }
        return defaultValue;
    }
}

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    // Windows offscreen 平台默认没有系统字体，显式加载中文字体，否则截图缺字。
    const int chineseFont = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    wpe_test::Report(chineseFont >= 0, "Chinese font loaded", __FILE__, __LINE__, QString());
    QFont appFont(QStringLiteral("Microsoft YaHei UI"));
    appFont.setPointSize(9);
    app.setFont(appFont);

    const QString shotsDir = Arg(argc, argv, "--shots", QStringLiteral(".codex-tmp/memwb-wpE/shots"));

    wpe_test::RunStoreTests();
    wpe_test::RunModelTests();
    wpe_test::RunPanelTests();
    wpe_test::RunPanelSurvivorTests();
    wpe_test::RunPanelDefectTests();
    // 第二轮修复（wave2）并入默认运行：审核者的补测 + 本轮新变异回归，不再靠环境变量开关。
    wpe_test::RunStoreWave2Tests();
    wpe_test::RunModelWave2Tests();
    wpe_test::RunPanelWave2Tests();
    wpe_test::RunPanelWave2MoreTests();
    wpe_test::RunWave2NewMutationTests();
    // 第三轮独立验证补测并入默认运行（第二轮修复者没有逐个重放审核者的变异，
    // 验证者重放后发现 8 个真实缺口，这些测试把它们钉住）。
    wpe_test::RunR3Tests();
    wpe_test::RunShotTests(shotsDir);

    std::cout << "wpE_tests: " << wpe_test::g_checks << " checks, "
              << wpe_test::g_failures << " failures" << std::endl;
    return wpe_test::g_failures == 0 ? 0 : 1;
}
