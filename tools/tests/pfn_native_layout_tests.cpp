#include "../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverPfn.h"
#include <cassert>
#include <iostream>

int main()
{
    using ksword::ark::pfnNativeLayoutSupported;
    static_assert(sizeof(void*) == 8, "This native-layout fixture targets x64.");
    assert(pfnNativeLayoutSupported(PROCESSOR_ARCHITECTURE_AMD64, 4096));
    for (unsigned short architecture : { static_cast<unsigned short>(PROCESSOR_ARCHITECTURE_INTEL),
        static_cast<unsigned short>(PROCESSOR_ARCHITECTURE_ARM64), static_cast<unsigned short>(PROCESSOR_ARCHITECTURE_UNKNOWN) }) {
        assert(!pfnNativeLayoutSupported(architecture, 4096));
    }
    for (unsigned long pageSize : {0UL, 512UL, 8192UL, 65536UL}) {
        assert(!pfnNativeLayoutSupported(PROCESSOR_ARCHITECTURE_AMD64, pageSize));
    }
    std::cout << "PFN_NATIVE_LAYOUT_TESTS=PASS\n";
}
