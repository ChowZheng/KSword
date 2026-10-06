// wpH_main.cpp
// 作用：WP-H（反汇编/文本/对比）离屏验证夹具的入口。
// 用法：wpH_tests.exe [--shots <目录>]
// 退出码：0 全部通过；1 存在失败断言。

#include "wpH_common.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>

#include <iostream>

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    // Windows offscreen 平台默认没有系统字体，必须显式加载中文字体与等宽字体，
    // 否则截图里的中文全部缺字（与 tools/memwb_ui/memwb_ui_tests.cpp 同一惯例）。
    const int chineseFont = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    const int monoFont = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/consola.ttf"));
    std::cerr << "[wpH] Chinese font loaded=" << (chineseFont >= 0) << " Consolas font loaded=" << (monoFont >= 0) << std::endl;
    QFont appFont(QStringLiteral("Microsoft YaHei UI"));
    appFont.setPointSize(9);
    app.setFont(appFont);

    QString shotsDir;
    for (int i = 1; i < argc; ++i)
    {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--shots") && i + 1 < argc)
        {
            shotsDir = QString::fromLocal8Bit(argv[++i]);
        }
    }

    wpH_test::ApplyTheme(false);
    wpH_test::RunDisasmTests(shotsDir);
    wpH_test::RunTextTests(shotsDir);
    wpH_test::RunCompareTests(shotsDir);
    // 修复波（wave2）新增：按审核报告 D1-D12 与可疑点逐条补的回归测试，见各文件头注释。
    wpH_test::RunDisasmRegressionTests();
    wpH_test::RunTextRegressionTests();
    wpH_test::RunCompareRegressionTests();
    // 第二轮独立审核（review2-wpH.md）补测：N1-N7 的 DEFECT 用例默认并入运行（不再靠环境
    // 变量开关），修复前应为红、修复后应为绿；并补齐了 18 回注 + 23 新变异对应的测试缺口。
    wpH_test::RunDisasmRegressionTests2();
    wpH_test::RunDisasmRegressionTests3();
    wpH_test::RunTextRegressionTests2();
    wpH_test::RunCompareRegressionTests2();
    // 提交前独立审核补测（见 wpH_tests.SExtra.cpp 头部），并入默认运行。
    wpH_test::RunSExtraTests();

    std::cerr << "[wpH] total checks=" << wpH_test::g_checks << " failures=" << wpH_test::g_failures << std::endl;
    return wpH_test::g_failures == 0 ? 0 : 1;
}
