#pragma once

// 基线窗口测试的共用支撑：构造小工具、确定性随机数、暴力参照实现。
//
// 暴力参照（Oracle）的写法刻意与被测实现完全不同：
//   * 被测实现：页序号区间 + 每侧有界扩展 + "左取心愿、右取余额、左再补足"三行让位；
//   * 参照实现：逐页判断合法/已落定 + 线性扫出整段连续区 + 把心愿值钳进可行区间。
// 两边只共享规格，不共享算法，所以随机对拍才有证明力。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryBaselineWindow.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <set>

namespace MemwbBaselineTests {

using ksword::memwb::AddressSpaceBounds;
using ksword::memwb::BaselineSpan;
using ksword::memwb::BaselineSpanStatus;
using ksword::memwb::BaselineWindowPolicy;

using PageSet = std::set<std::uint64_t>;

// uint64 的最大值，测试里到处用来检验"没有被截断/回绕"。
inline constexpr std::uint64_t kMax64 = 0xFFFFFFFFFFFFFFFFULL;

// MakePolicy：构造一份页大小与页数上限都显式给出的策略。
inline BaselineWindowPolicy MakePolicy(const std::uint64_t pageSize, const std::uint64_t maxPages) {
    BaselineWindowPolicy policy;
    policy.pageSize = pageSize;
    policy.maxPages = maxPages;
    return policy;
}

// MakeBounds：构造地址空间闭区间 [lowest, highest]。
inline AddressSpaceBounds MakeBounds(const std::uint64_t lowest, const std::uint64_t highest) {
    AddressSpaceBounds bounds;
    bounds.lowest = lowest;
    bounds.highest = highest;
    return bounds;
}

// MakePages：由页起点列表构造已落定页集合。
inline PageSet MakePages(const std::initializer_list<std::uint64_t> starts) {
    return PageSet(starts);
}

// MakeRun：构造 count 个连续页的集合，第 i 页起点 = firstStart + i * pageSize。
inline PageSet MakeRun(const std::uint64_t firstStart, const std::uint64_t count, const std::uint64_t pageSize) {
    PageSet pages;
    for (std::uint64_t index = 0; index < count; ++index) {
        pages.insert(firstStart + index * pageSize);
    }
    return pages;
}

// Select4K：用 4 KiB 页、给定上限、整个 uint64 空间选取，绝大多数用例走这条。
inline BaselineSpan Select4K(const std::uint64_t insertion, const PageSet& pages, const std::uint64_t maxPages) {
    return ksword::memwb::SelectBaselineSpan(insertion, pages, MakeBounds(0, kMax64), MakePolicy(4096, maxPages));
}

// ExpectSpan：断言选取结果的状态、起点与长度三者同时符合手算值。
inline void ExpectSpan(
    KswordTests::Suite& suite,
    const BaselineSpan& span,
    const BaselineSpanStatus status,
    const std::uint64_t base,
    const std::uint64_t length,
    const wchar_t* label) {
    suite.expect(span.status == status && span.base == base && span.length == length, label);
}

// Rng：确定性伪随机数（splitmix64），跨编译器结果一致，失败可复现。
class Rng {
public:
    explicit Rng(const std::uint64_t seed) : state_(seed) {}

    // 返回 [0, bound) 内的数；bound 必须非零。
    std::uint64_t Below(const std::uint64_t bound) {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        value ^= value >> 31;
        return value % bound;
    }

private:
    std::uint64_t state_;
};

// Oracle：暴力参照的"世界观"——一页是否合法、是否已落定、是否可入选。
struct Oracle {
    // 已落定页起点集合（引用，生命周期由调用方保证）。
    const PageSet& settled;
    // 地址空间闭区间。
    std::uint64_t lowest;
    std::uint64_t highest;
    // 页大小。
    std::uint64_t pageSize;

    // 页序号 index 是否合法：整页在 [lowest, highest] 内，且末字节不是 0xFF..FF。
    bool Valid(const std::uint64_t index) const {
        const std::uint64_t start = index * pageSize;
        const std::uint64_t last = start + (pageSize - 1);
        return start >= lowest && last <= highest && last != kMax64;
    }

    // 页序号 index 是否已落定。
    bool Settled(const std::uint64_t index) const {
        return settled.count(index * pageSize) != 0;
    }

    // 页序号 index 是否可入选：合法且已落定。
    bool Usable(const std::uint64_t index) const {
        return Valid(index) && Settled(index);
    }
};

// ExpectedSpan：参照结果。
struct ExpectedSpan {
    BaselineSpanStatus status = BaselineSpanStatus::InvalidArgument;
    std::uint64_t base = 0;
    std::uint64_t length = 0;
};

// ComputeExpected：用暴力参照算出期望的跨度（策略合法的前提下）。
inline ExpectedSpan ComputeExpected(const Oracle& oracle, const std::uint64_t insertion, const std::uint64_t maxPages) {
    ExpectedSpan expected;
    if (oracle.lowest > oracle.highest) {
        return expected;
    }
    const std::uint64_t pivot = insertion / oracle.pageSize;
    if (!oracle.Valid(pivot)) {
        expected.status = BaselineSpanStatus::InsertionOutsideSpace;
        return expected;
    }
    if (!oracle.Settled(pivot)) {
        expected.status = BaselineSpanStatus::InsertionPageNotSettled;
        return expected;
    }

    // 线性扫出完整的连续可用段 [low, high]。
    std::uint64_t low = pivot;
    while (low > 0 && oracle.Usable(low - 1)) {
        --low;
    }
    std::uint64_t high = pivot;
    const std::uint64_t topIndex = kMax64 / oracle.pageSize;
    while (high < topIndex && oracle.Usable(high + 1)) {
        ++high;
    }

    // 段不超过上限就整段返回；否则在所有"恰好 maxPages 页且含 pivot"的窗口里，
    // 取左侧页数最接近 (maxPages-1)/2 的那个，即把心愿值钳进可行区间 [leftMin, leftMax]。
    std::uint64_t first = low;
    std::uint64_t count = high - low + 1;
    if (count > maxPages) {
        const std::uint64_t wanted = (maxPages - 1) / 2;
        const std::uint64_t leftMin = (maxPages - 1 > high - pivot) ? (maxPages - 1) - (high - pivot) : 0;
        const std::uint64_t leftMax = (pivot - low < maxPages - 1) ? (pivot - low) : maxPages - 1;
        std::uint64_t left = wanted;
        if (left < leftMin) {
            left = leftMin;
        }
        if (left > leftMax) {
            left = leftMax;
        }
        first = pivot - left;
        count = maxPages;
    }
    expected.status = BaselineSpanStatus::Ok;
    expected.base = first * oracle.pageSize;
    expected.length = count * oracle.pageSize;
    return expected;
}

// CheckSpanProperties：不依赖参照实现，直接按规格检查一个 Ok 跨度的性质：
// 非空、页对齐、不超上限、不回绕、含插入点、每一页都可入选（已落定且合法）。
inline bool CheckSpanProperties(
    const BaselineSpan& span,
    const Oracle& oracle,
    const std::uint64_t insertion,
    const std::uint64_t maxPages) {
    if (span.length == 0 || span.base % oracle.pageSize != 0 || span.length % oracle.pageSize != 0) {
        return false;
    }
    if (span.length / oracle.pageSize > maxPages || span.length > kMax64 - span.base) {
        return false;
    }
    if (!span.Contains(insertion)) {
        return false;
    }
    for (std::uint64_t offset = 0; offset < span.length; offset += oracle.pageSize) {
        if (!oracle.Usable((span.base + offset) / oracle.pageSize)) {
            return false;
        }
    }
    return true;
}

// RunRandomRegion：在以 regionBase 起的 64 页（每页 16 字节）区域上随机对拍 iterations 次。
// 每轮随机生成已落定集合（含区域下方的页与不对齐杂项）、地址空间边界、页数上限与插入点，
// 同时核对"与暴力参照一致"与"规格性质成立"，并断言对拍没有空转。
inline void RunRandomRegion(
    KswordTests::Suite& suite,
    const std::uint64_t regionBase,
    const std::uint64_t seed,
    const int iterations,
    const wchar_t* label) {
    constexpr std::uint64_t kPage = 16;
    constexpr std::uint64_t kPages = 64;
    constexpr std::uint64_t kRegionBytes = kPage * kPages;
    Rng rng(seed);
    int mismatches = 0;
    int propertyFailures = 0;
    int okCount = 0;
    int clippedCount = 0;

    for (int iteration = 0; iteration < iterations; ++iteration) {
        // 已落定集合：每页按密度随机取，再混入区域下方的页与不对齐的杂项。
        static const std::uint64_t densities[] = { 30, 70, 90, 100 };
        const std::uint64_t density = densities[rng.Below(4)];
        PageSet settled;
        for (std::uint64_t page = 0; page < kPages; ++page) {
            if (rng.Below(100) < density) {
                settled.insert(regionBase + page * kPage);
            }
        }
        settled.insert(regionBase - kPage);
        settled.insert(regionBase - 2 * kPage);
        settled.insert((regionBase + rng.Below(kRegionBytes)) | 1);

        // 地址空间边界：下界在区域下方 20 字节到区域内 200 字节之间，
        // 上界在区域末尾下方 200 字节到上方 20 字节之间（顶端区域会被钳到最大值）。
        const std::uint64_t lowest = regionBase - 20 + rng.Below(221);
        const std::uint64_t highBase = regionBase + kRegionBytes - 1 - 200;
        std::uint64_t highest = highBase + rng.Below(221);
        if (highest < highBase) {
            highest = kMax64;
        }

        // 页数上限 1..69，插入点取区域内外一圈（顶端区域越过最大值的回绕值钳回最大值）。
        const std::uint64_t maxPages = 1 + rng.Below(69);
        std::uint64_t insertion = regionBase - 20 + rng.Below(kRegionBytes + 40);
        if (insertion < regionBase - 20) {
            insertion = kMax64;
        }

        const Oracle oracle{ settled, lowest, highest, kPage };
        const ExpectedSpan expected = ComputeExpected(oracle, insertion, maxPages);
        const BaselineSpan actual = ksword::memwb::SelectBaselineSpan(
            insertion, settled, MakeBounds(lowest, highest), MakePolicy(kPage, maxPages));

        if (actual.status != expected.status || actual.base != expected.base || actual.length != expected.length) {
            ++mismatches;
        }
        if (actual.status == BaselineSpanStatus::Ok) {
            ++okCount;
            if (!CheckSpanProperties(actual, oracle, insertion, maxPages)) {
                ++propertyFailures;
            }
            if (actual.length / kPage == maxPages) {
                ++clippedCount;
            }
        } else if (actual.base != 0 || actual.length != 0) {
            // 失败路径上的跨度必须保持安全初值。
            ++propertyFailures;
        }
    }

    suite.expect(mismatches == 0, label);
    suite.expect(propertyFailures == 0, L"random: every Ok span satisfies the specification properties");
    // 防止对拍空转：必须真的产生过大量成功跨度，也真的走到过"被上限裁剪"的分支。
    suite.expect(okCount > iterations / 4, L"random: the fixture produced plenty of successful spans");
    suite.expect(clippedCount > iterations / 50, L"random: the fixture exercised the clipping branch");
}

} // namespace MemwbBaselineTests
