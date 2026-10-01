#include "../MemoryLifetime.h"
#include <cstdio>
bool controlProtocolTests();

int main()
{
    using namespace ksword::titan;
    const auto maximum = (std::numeric_limits<std::uintptr_t>::max)();
    const bool okay =
        decommitCovers(0x2011, 1, 0x2000, 0x1000) &&
        decommitCovers(0x2011, 1, 0x2FFF, 0x1000) &&
        !decommitCovers(0x2011, 1, 0x3000, 0x1000) &&
        !decommitCovers(0x2011, 1, 0x1FFF, 0x1000) &&
        decommitCovers(0x2FFF, 2, 0x3FFF, 0x1000) &&
        !decommitCovers(0x2FFF, 2, 0x4000, 0x1000) &&
        !decommitCovers(maximum - 2, 4, maximum, 0x1000) &&
        decommitCovers(maximum - 2, 3, maximum, 0x1000) &&
        !decommitCovers(0x2000, 0, 0x2000, 0x1000) &&
        moduleOwnsBinding(true, 10, 0x5000, 10, 0x5000) &&
        !moduleOwnsBinding(false, 10, 0x5000, 10, 0x5000) &&
        !moduleOwnsBinding(true, 11, 0x5000, 10, 0x5000) &&
        !moduleOwnsBinding(true, 10, 0x6000, 10, 0x5000) &&
        !moduleOwnsBinding(true, 10, 0, 10, 0);
    std::puts(okay ? "PASS: decommit page rounding/overflow and module owner isolation" : "FAIL: lifetime coverage");
    return okay && controlProtocolTests() ? 0 : 1;
}
