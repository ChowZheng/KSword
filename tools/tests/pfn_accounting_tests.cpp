#include "../../shared/evidence/PfnAccounting.h"
#include <cassert>
#include <iostream>

int main()
{
    using namespace ksword::pfn;
    std::vector<Range> ranges{{10, 10}, {5, 10}, {20, 2}, {100, 1}, {5, 10}};
    assert(normalizeRanges(ranges));
    assert(ranges.size() == 2 && ranges[0].first == 5 && ranges[0].count == 17);
    assert(ranges[1].first == 100 && ranges[1].count == 1);
    std::vector<Range> overflow{{(1ULL << 40) - 1, 2}};
    assert(!normalizeRanges(overflow));
    std::vector<Range> zero{{0, 0}};
    assert(!normalizeRanges(zero));

    Accounting counts;
    counts.expected = 16 * 8 + 7;
    for (unsigned use = 0; use < 16; ++use) {
        for (unsigned list = 0; list < 8; ++list) {
            const Identity page{use | (static_cast<std::uint64_t>(list) << 4), 0};
            counts.add(page, classify(page));
        }
    }
    counts.failed(3);
    assert(counts.notScanned() == 4 && counts.unreadable == 3);
    assert(counts.availablePages == 48 && counts.bad() == 16);
    assert(counts.inUse(Use::Unknown) == 16); // Future use codes stay unknown.
    assert(counts.reconciles()); // Partial scan retains the complete denominator.
    counts.add({~0ULL, 0}, Use::Unknown);
    assert(counts.reconciles() && counts.unreadable == 4 && counts.notScanned() == 3);

    const Identity image{1 | (6ULL << 4), 0xffff800000010001ULL};
    assert(classify(image) == Use::Image);
    assert(classify({0 | (6ULL << 4), 0}, true) == Use::Compression);
    assert(classify({15 | (0ULL << 4), 0}) == Use::Unassigned); // Free-list use bits carry no owner.
    const Identity pinned{5 | (6ULL << 4) | (1ULL << 8) | (1ULL << 60), 0};
    Accounting attributes;
    attributes.expected = 1;
    attributes.add(pinned, classify(pinned));
    assert(attributes.inUse(Use::NonpagedPool) == 1 && attributes.pinned == 1 && attributes.nonTradeable == 1);
    assert(attributes.reconciles()); // Attributes do not create additional pages.
    attributes.expected = 2;
    assert(attributes.notScanned() == 1 && attributes.reconciles());
    std::cout << "PFN_ACCOUNTING_TESTS=PASS\n";
}
