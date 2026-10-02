#include "../shared/evidence/MemoryAddressInput.h"

#include <cstdio>
#include <cstdlib>

int main()
{
    struct Valid { const char* text; std::uint64_t value; };
    const Valid valid[] = {
        {"0", 0}, {"0x1234", 0x1234}, {"0XABCdef", 0xABCDEF},
        {" \tfffff800`12345678\r\n", 0xFFFFF80012345678ULL},
        {"7fff_ffff_ffff_ffff", 0x7FFFFFFFFFFFFFFFULL},
        {"FFFFFFFFFFFFFFFF", UINT64_MAX},
        {"000000000000000000000000000001", 1}
    };
    const char* invalid[] = {
        "", " \t", "0x", "-1", "+1", "10000000000000000",
        "0xFFFFFFFFFFFFFFFFF", "1234 5678", "1234\n5678",
        "1234g", "0x0x12", "`1234", "1234`", "12__34", "12`_34"
    };
    for (const auto& sample : valid)
    {
        std::uint64_t result = 0;
        if (!Ksword::Evidence::ParseHexAddress(sample.text, result) || result != sample.value)
        {
            std::fprintf(stderr, "Rejected or misparsed valid address: %s\n", sample.text);
            return EXIT_FAILURE;
        }
    }
    for (const char* text : invalid)
    {
        std::uint64_t result = 0x12345678;
        if (Ksword::Evidence::ParseHexAddress(text, result) || result != 0x12345678)
        {
            std::fprintf(stderr, "Accepted invalid address or changed output: %s\n", text);
            return EXIT_FAILURE;
        }
    }
    std::puts("Memory address input: 22 cases passed.");
    return EXIT_SUCCESS;
}
