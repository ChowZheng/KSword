// ============================================================
// wpK1_common.cpp
// 作用：实现 wpK1_common.h 的断言计数。
// ============================================================

#include "wpK1_common.h"

#include <cstdio>

namespace wpK1_test
{
    int g_checks = 0;
    int g_failures = 0;

    void Report(
        const bool ok,
        const char* const expression,
        const char* const file,
        const int line,
        const std::string& note)
    {
        ++g_checks;
        if (ok)
        {
            return;
        }
        ++g_failures;
        std::fprintf(
            stderr,
            "[FAIL] %s(%d): %s%s%s\n",
            file,
            line,
            expression,
            note.empty() ? "" : " -- ",
            note.empty() ? "" : note.c_str());
    }
}
