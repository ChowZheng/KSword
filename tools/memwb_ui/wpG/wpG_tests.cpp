// ============================================================
// wpG_tests.cpp
// 作用：WP-G 离屏验证夹具的入口。依次跑四组测试，最后打印汇总行
// "memwb_ui_tests: N checks"（与仓库其它 memwb_ui 夹具的汇总格式一致，
// 方便人工/脚本从输出里提取数字），非零失败数时退出码为 1。
// ============================================================

#include "wpG_common.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>

#include <cstdio>

int main(int argc, char** argv)
{
    // 离屏平台：没有显式指定时强制 offscreen，保证无人值守也能跑。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    // 字体：Windows offscreen 平台默认没有系统字体，必须显式加载中文字体，
    // 否则截图里的全部中文都会变成缺字方块（本夹具第一次构建就踩过这个坑）。
    const int chineseFont = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    wpg_test::Report(chineseFont >= 0, "Chinese font loaded", __FILE__, __LINE__, QString());
    QFont appFont(QStringLiteral("Microsoft YaHei UI"));
    appFont.setPointSize(9);
    app.setFont(appFont);

    QString shotsDir = QStringLiteral("shots");
    for (int i = 1; i < argc; ++i)
    {
        if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--shots") && i + 1 < argc)
        {
            shotsDir = QString::fromLocal8Bit(argv[i + 1]);
            ++i;
        }
    }

    wpg_test::RunSessionBarTests();
    wpg_test::RunStatusBarTests();
    wpg_test::RunConfirmationsTests();
    wpg_test::RunGapTests();
    wpg_test::RunGapTests2();
    wpg_test::RunShotsTests(shotsDir);
    // RunReview2Tests 放在截图之后：其中的 i18n 探测会把 LanguageManager 切到
    // en-US 且不会切回去，放在截图之前会让后续截图的中文文案变成英文（见
    // wpG_common.h 对应声明处的说明）。
    wpg_test::RunReview2Tests();

    std::printf("memwb_ui_tests: %d checks\n", wpg_test::g_checks);
    if (wpg_test::g_failures > 0)
    {
        std::fprintf(stderr, "wpG_tests: %d/%d checks FAILED\n", wpg_test::g_failures, wpg_test::g_checks);
        return 1;
    }
    std::printf("wpG_tests: all %d checks passed\n", wpg_test::g_checks);
    return 0;
}
