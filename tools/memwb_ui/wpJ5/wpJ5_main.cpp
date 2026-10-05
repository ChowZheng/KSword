// ============================================================
// wpJ5_main.cpp
// 作用：WP-J5（WorkbenchHexPane）离屏验证夹具的入口。链接真实 HexCanvas 家族
// （QWidget 子类），因此用 QApplication 而不是 QCoreApplication，配合 offscreen
// 平台插件在无显示环境下运行。
//
// 用法：wpJ5_tests.exe
// 退出码：0 全部通过；1 有断言失败。终端汇总行恰好是
//         "wpJ5_tests: N checks, M failures"（跑法脚本据此解析）。
// ============================================================

#include "wpJ5_common.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>

#include <iostream>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    // 字体：Windows offscreen 平台默认没有系统字体，不显式加载会让截图里全部
    // 中文变成缺字方块（wpG/wpE 两个既有夹具已经踩过同一个坑并留下了同样的
    // 修法，这里照抄：加载系统的微软雅黑，找不到就报出来而不是静默吞掉——
    // "亲自看截图"这一步如果中文本身是方块，看了也等于没看）。
    const int chineseFontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    if (chineseFontId < 0)
    {
        std::cout << "wpJ5_tests: 警告——未能加载中文字体 C:/Windows/Fonts/msyh.ttc，"
                      "截图中的中文可能显示为缺字方块（已继续运行，不影响断言结果）"
                   << std::endl;
    }
    else
    {
        QFont appFont(QStringLiteral("Microsoft YaHei UI"));
        appFont.setPointSize(9);
        app.setFont(appFont);
    }

    wpj5_test::RunWiringTests();
    wpj5_test::RunBaselineTests();
    wpj5_test::RunWriteTests();
    wpj5_test::RunFindTests();
    wpj5_test::RunNavigationTests();
    wpj5_test::RunVisualTests();

    std::cout << "wpJ5_tests: " << wpj5_test::g_checks << " checks, "
              << wpj5_test::g_failures << " failures" << std::endl;
    return wpj5_test::g_failures == 0 ? 0 : 1;
}
