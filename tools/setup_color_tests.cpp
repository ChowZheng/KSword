// 调用安装器生产配色函数，不创建窗口、不运行安装业务。
#include "../KswordSetup/KswordGUI/KTheme.h"
#include <cstdio>

int main()
{
    const Fl_Color backgrounds[] = {fl_rgb_color(0, 100, 251), fl_rgb_color(46, 128, 252),
        fl_rgb_color(0, 80, 201), FL_WHITE, FL_BLACK, fl_rgb_color(128, 128, 128)};
    int failures = 0;
    int checks = 0;
    for (const Fl_Color background : backgrounds)
    {
        for (const Fl_Color seed : {FL_WHITE, FL_BLACK})
        {
            ++checks;
            if (KThemeContrast(KThemeReadableText(seed, background), background) < 4.5)
            {
                ++failures;
            }
        }
    }
    ++checks;
    failures += KThemeReadableText(FL_WHITE, backgrounds[0]) != FL_WHITE;
    ++checks;
    failures += KThemeReadableText(FL_WHITE, backgrounds[1]) == FL_WHITE;
    std::printf("SETUP_COLOR_ASSERTIONS=%d FAILURES=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
