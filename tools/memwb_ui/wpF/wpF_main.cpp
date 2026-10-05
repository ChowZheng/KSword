// wpF_main.cpp
// 作用：WP-F（int3 补丁：Int3Controller + Int3PatchPanel）离屏验证夹具的入口。
// 用法：memwb_wpF_tests.exe [--shots <目录>]
// 退出码：0 全部通过；1 有断言失败。

#include "wpF_tests_common.h"

#include <QApplication>
#include <QDir>
#include <QFont>
#include <QFontDatabase>

#include <iostream>

namespace
{
    // Arg：读取命令行里 --name value 形式的参数，找不到返回默认值。
    QString Arg(const int argc, char** argv, const char* name, const QString& defaultValue)
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
    // 离屏平台：没有显式指定时强制 offscreen，保证无人值守也能跑。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    // Windows offscreen 平台默认没有系统字体，显式加载中文字体，否则截图里的中文缺字。
    const int chineseFont = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    wpf_test::Report(chineseFont >= 0, "Chinese font loaded", __FILE__, __LINE__, QString());
    QFont appFont(QStringLiteral("Microsoft YaHei UI"));
    appFont.setPointSize(9);
    app.setFont(appFont);

    const QString shotsDir = Arg(argc, argv, "--shots", QStringLiteral(".codex-tmp/memwb-wpF/shots"));
    QDir().mkpath(shotsDir);

    wpf_test::RunControllerTests();
    wpf_test::RunLeavePromptTests();
    wpf_test::RunPanelTests(shotsDir);

    std::cout << "wpF_tests: " << wpf_test::g_checks << " checks, "
              << wpf_test::g_failures << " failures" << std::endl;
    return wpf_test::g_failures == 0 ? 0 : 1;
}
