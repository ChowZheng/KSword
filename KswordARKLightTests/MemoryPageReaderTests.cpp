// 页读取器（shared/evidence/memory_workbench/MemoryPageReader.h）的离线测试。
//
// 为什么这个模块值得一整套按调用顺序逐条断言的测试：它的全部行为是"怎么切块、
// 遇到四种读取状态分别怎么处理、什么时候停止"，这些规则错一条都不会抛异常，
// 只会在界面上表现成"某一页该显示问号却显示了数据"或者"明明还有别的页可以读
// 却提前放弃了"。只断言最终的 PageRecord 还不够，必须同时断言假端口记录到的
// (address,length) 调用序列，才能钉死"已定位的页不重复探测，批量零字节失败
// 须核验同址单页并逐页完成当前块"这条规则本身。
//
// 断言原则与 MemoryBaselineWindowTests.cpp 一致：
//   * 每个场景的 script 与期望调用序列全部手算写死；
//   * 边界两侧都测（上限 -1/恰好/+1 页、地址空间末端能/不能表示）；
//   * 四种读取状态、取消、通道失败、暂存区弄脏都必须有专门的场景，不能只覆盖
//     "一路顺利"的路径；
//   * 每个无效字节必须同时核对 bytes==0 与 valid==0，不能只看 state。

#include "TestSupport.h"

#include "MemoryIoTestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryPageReader.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace {

using namespace MemwbIoTests;

using ksword::memwb::HexViewport;
using ksword::memwb::PageRecord;
using ksword::memwb::PageReadResult;
using ksword::memwb::PageState;
using ksword::memwb::ReadPages;

// kPage：本文件统一引用 HexViewport::kPageBytes，不另起一个数字字面量。
constexpr std::uint64_t kPage = HexViewport::kPageBytes;

// ------------------------------------------------------------
// 小工具：切片、全等判断、三种页状态各自的断言封装。
// 这些封装只负责"少打重复代码"，不做任何被测逻辑，期望值仍然是调用方手算的。
// ------------------------------------------------------------

// Slice：取 src 的 [offset, offset+length)，供测试按偏移核对假端口返回的数据。
Bytes Slice(const Bytes& src, std::size_t offset, std::size_t length) {
    return Bytes(src.begin() + static_cast<std::ptrdiff_t>(offset),
        src.begin() + static_cast<std::ptrdiff_t>(offset + length));
}

// AllEqual：某个字节向量里是否每个元素都等于 value。bytes 与 valid 都是
// vector<uint8_t>，同一个函数够用。
bool AllEqual(const std::vector<std::uint8_t>& data, std::uint8_t value) {
    for (const std::uint8_t byte : data) {
        if (byte != value) {
            return false;
        }
    }
    return true;
}

// ExpectFullyValid：核对一页"整页真实读到"：地址对、状态 Valid、字节与期望逐一
// 相等、有效位全 1。
void ExpectFullyValid(
    KswordTests::Suite& suite,
    const std::wstring& label,
    const PageRecord& record,
    std::uint64_t expectedPageStart,
    const Bytes& expectedBytes) {
    suite.expect(record.pageStart == expectedPageStart && record.state == PageState::Valid,
        (label + L": page start and state").c_str());
    suite.expect(record.bytes == expectedBytes, (label + L": bytes equal the data the port returned").c_str());
    suite.expect(AllEqual(record.valid, 1), (label + L": every byte of a Valid page is marked valid").c_str());
}

// ExpectBlankPage：核对一页"完全不可信"：bytes 与 valid 全 0，状态是调用方指定
// 的 Unreadable 或 NotAttempted（两者的字节表现相同，区别只在"有没有问过"）。
void ExpectBlankPage(
    KswordTests::Suite& suite,
    const std::wstring& label,
    const PageRecord& record,
    std::uint64_t expectedPageStart,
    PageState expectedState) {
    suite.expect(record.pageStart == expectedPageStart && record.state == expectedState,
        (label + L": page start and state").c_str());
    suite.expect(AllEqual(record.bytes, 0) && AllEqual(record.valid, 0),
        (label + L": every byte of a blank page is bytes=0 and valid=0").c_str());
}

// ExpectPartiallyValid：核对一页"前 validPrefixLen 字节真实读到，其余全无效"：
// 前缀与期望逐一相等、前缀有效位全 1；尾部 bytes 与 valid 全 0。
void ExpectPartiallyValid(
    KswordTests::Suite& suite,
    const std::wstring& label,
    const PageRecord& record,
    std::uint64_t expectedPageStart,
    std::uint64_t validPrefixLen,
    const Bytes& expectedPrefixBytes) {
    const std::size_t prefixLen = static_cast<std::size_t>(validPrefixLen);
    suite.expect(record.pageStart == expectedPageStart && record.state == PageState::PartiallyValid,
        (label + L": page start and state").c_str());
    suite.expect(Slice(record.bytes, 0, prefixLen) == expectedPrefixBytes,
        (label + L": the valid prefix bytes match what the port returned").c_str());
    suite.expect(AllEqual(Slice(record.valid, 0, prefixLen), 1),
        (label + L": the valid prefix is marked valid").c_str());
    suite.expect(AllEqual(Slice(record.bytes, prefixLen, record.bytes.size() - prefixLen), 0)
        && AllEqual(Slice(record.valid, prefixLen, record.valid.size() - prefixLen), 0),
        (label + L": the rest of a PartiallyValid page is bytes=0 and valid=0").c_str());
}

// CallsMatch：fake 记录到的调用序列是否与手算的 (address,length) 列表逐一相等
// （包括个数）。这是钉死读取预算与核验范围的关键断言，所以单独抽出来，调用处
// 直接把期望写成字面量列表，一眼就能对着算法手算。
bool CallsMatch(
    const std::vector<ReadCall>& calls,
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& expected) {
    if (calls.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < calls.size(); ++index) {
        if (calls[index].address != expected[index].first || calls[index].length != expected[index].second) {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------
// 一、全 Ok：单块、单次调用，三页全部真实读到。
// ------------------------------------------------------------
void TestFullOk(KswordTests::Suite& suite) {
    constexpr std::uint64_t kBase = 0x100000ULL;
    const Bytes data = MakePattern(0x10, static_cast<std::size_t>(3 * kPage));

    FakeMemoryIoPort fake;
    fake.limits.maxReadBytes = 0; // 不限，整段范围是一块。
    fake.script = { MakeOk(data) };

    const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 3, nullptr);

    suite.expect(CallsMatch(fake.calls, { { kBase, 3 * kPage } }),
        L"page reader: a single unlimited block issues exactly one Read covering all three pages");
    suite.expect(result.portCalls == 1 && !result.channelFailed && !result.cancelled
        && !result.scratchAreaDirty && !result.readModifyWriteWindow && result.failure.empty(),
        L"page reader: a full Ok leaves every status flag at its safe default");
    suite.expect(result.note.empty() && result.unreadableReason.empty(),
        L"page reader: a full Ok with no port-reported notes leaves note/unreadableReason at their safe default (P-1)");
    suite.expect(result.pages.size() == 3, L"page reader: the result holds exactly the requested page count");

    for (std::uint64_t page = 0; page < 3; ++page) {
        ExpectFullyValid(suite, L"page reader: full Ok page " + std::to_wstring(page),
            result.pages[static_cast<std::size_t>(page)], kBase + page * kPage,
            Slice(data, static_cast<std::size_t>(page * kPage), static_cast<std::size_t>(kPage)));
    }
}

// ------------------------------------------------------------
// 二、前缀 Partial：失败点所在页从失败点到页尾标无效，下一页起继续读该块剩余部分。
// ------------------------------------------------------------
void TestPartialContinuesFromNextPage(KswordTests::Suite& suite) {
    constexpr std::uint64_t kBase = 0x5000ULL;
    // 第一次 Read 返回"第 0 页整页 + 第 1 页前 10 字节"，之后的字节没读到。
    const Bytes partialData = MakePattern(0x20, static_cast<std::size_t>(kPage) + 10);
    const Bytes tailData = MakePattern(0x77, static_cast<std::size_t>(kPage));

    FakeMemoryIoPort fake;
    fake.limits.maxReadBytes = 0;
    fake.script = { MakePartial(partialData), MakeOk(tailData) };

    const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 3, nullptr);

    suite.expect(CallsMatch(fake.calls, { { kBase, 3 * kPage }, { kBase + 2 * kPage, 1 * kPage } }),
        L"page reader: a partial prefix is followed by exactly one continuation Read for the rest of the block");
    suite.expect(result.portCalls == 2 && !result.channelFailed && !result.cancelled,
        L"page reader: the block completes in two calls without failing or cancelling");

    ExpectFullyValid(suite, L"page reader: partial scenario page 0", result.pages[0], kBase,
        Slice(partialData, 0, static_cast<std::size_t>(kPage)));
    ExpectPartiallyValid(suite, L"page reader: partial scenario page 1", result.pages[1], kBase + kPage, 10,
        Slice(partialData, static_cast<std::size_t>(kPage), 10));
    ExpectFullyValid(suite, L"page reader: partial scenario page 2", result.pages[2], kBase + 2 * kPage, tailData);
}

// ------------------------------------------------------------
// 三、Unreadable：批量失败不能定位空洞，核验每一页后再标无效。
// ------------------------------------------------------------
void TestUnreadableThenContinue(KswordTests::Suite& suite) {
    constexpr std::uint64_t kBase = 0x9000ULL;
    const Bytes tailData = MakePattern(0x55, static_cast<std::size_t>(2 * kPage));

    FakeMemoryIoPort fake;
    fake.limits.maxReadBytes = 0;
    fake.script = { MakeUnreadable("bulk unreadable"), MakeUnreadable("page 0 is paged out"),
        MakeOk(Slice(tailData, 0, static_cast<std::size_t>(kPage))),
        MakeOk(Slice(tailData, static_cast<std::size_t>(kPage), static_cast<std::size_t>(kPage))) };

    const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 3, nullptr);

    suite.expect(CallsMatch(fake.calls, { { kBase, 3 * kPage }, { kBase, kPage },
        { kBase + kPage, kPage }, { kBase + 2 * kPage, kPage } }),
        L"page reader: an ambiguous bulk failure is followed by one attempt per page in the same block");
    suite.expect(result.portCalls == 4 && !result.channelFailed, L"page reader: Unreadable does not stop the block");

    ExpectBlankPage(suite, L"page reader: unreadable scenario page 0", result.pages[0], kBase, PageState::Unreadable);
    ExpectFullyValid(suite, L"page reader: unreadable scenario page 1", result.pages[1], kBase + kPage,
        Slice(tailData, 0, static_cast<std::size_t>(kPage)));
    ExpectFullyValid(suite, L"page reader: unreadable scenario page 2", result.pages[2], kBase + 2 * kPage,
        Slice(tailData, static_cast<std::size_t>(kPage), static_cast<std::size_t>(kPage)));
}

// ------------------------------------------------------------
// 四、中间有空洞多处：Unreadable / Partial / Unreadable / Ok 交替，五页一块。
// ------------------------------------------------------------
void TestMultipleHoles(KswordTests::Suite& suite) {
    constexpr std::uint64_t kBase = 0xC000ULL;
    const Bytes tinyPartial = MakePattern(0x30, 10);
    const Bytes okTail = MakePattern(0x90, static_cast<std::size_t>(2 * kPage));

    FakeMemoryIoPort fake;
    fake.limits.maxReadBytes = 0;
    fake.script = {
        MakeUnreadable("bulk hole location unknown"),
        MakeUnreadable("page 0 hole"),
        MakePartial(tinyPartial),
        MakeUnreadable("page 2 hole"),
        MakeOk(Slice(okTail, 0, static_cast<std::size_t>(kPage))),
        MakeOk(Slice(okTail, static_cast<std::size_t>(kPage), static_cast<std::size_t>(kPage))),
    };

    const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 5, nullptr);

    suite.expect(CallsMatch(fake.calls,
        { { kBase, 5 * kPage }, { kBase, kPage }, { kBase + kPage, kPage },
            { kBase + 2 * kPage, kPage }, { kBase + 3 * kPage, kPage }, { kBase + 4 * kPage, kPage } }),
        L"page reader: one ambiguous failure plus five single pages produces a bounded call sequence");
    suite.expect(result.portCalls == 6 && !result.channelFailed, L"page reader: multiple holes never stop the block");

    ExpectBlankPage(suite, L"page reader: holes scenario page 0", result.pages[0], kBase, PageState::Unreadable);
    ExpectPartiallyValid(suite, L"page reader: holes scenario page 1", result.pages[1], kBase + kPage, 10, tinyPartial);
    ExpectBlankPage(suite, L"page reader: holes scenario page 2", result.pages[2], kBase + 2 * kPage, PageState::Unreadable);
    ExpectFullyValid(suite, L"page reader: holes scenario page 3", result.pages[3], kBase + 3 * kPage,
        Slice(okTail, 0, static_cast<std::size_t>(kPage)));
    ExpectFullyValid(suite, L"page reader: holes scenario page 4", result.pages[4], kBase + 4 * kPage,
        Slice(okTail, static_cast<std::size_t>(kPage), static_cast<std::size_t>(kPage)));
}

// ------------------------------------------------------------
// 五、Failed：立即整体停止，当前位置之后（含本块剩余与后面所有块）全部 NotAttempted。
// ------------------------------------------------------------
void TestFailedStopsEverything(KswordTests::Suite& suite) {
    // 场景 A：第一块第一次 Read 就失败，四页全部没问过。
    {
        constexpr std::uint64_t kBase = 0x20000ULL;
        FakeMemoryIoPort fake;
        fake.limits.maxReadBytes = 0;
        fake.script = { MakeFailed("channel offline") };

        const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 4, nullptr);

        suite.expect(result.channelFailed && result.failure == "channel offline" && result.portCalls == 1,
            L"page reader: an immediate Failed reports the port's failure text and stops after one call");
        for (std::uint64_t page = 0; page < 4; ++page) {
            ExpectBlankPage(suite, L"page reader: failed-immediately page " + std::to_wstring(page),
                result.pages[static_cast<std::size_t>(page)], kBase + page * kPage, PageState::NotAttempted);
        }
    }

    // 场景 B：第一块成功，第二块失败——已经写进结果的页保留，之后的页 NotAttempted。
    {
        constexpr std::uint64_t kBase = 0x40000ULL;
        const Bytes okData = MakePattern(0xAA, static_cast<std::size_t>(2 * kPage));

        FakeMemoryIoPort fake;
        fake.limits.maxReadBytes = 2 * kPage; // 两页一块。
        fake.script = { MakeOk(okData), MakeFailed("disk gone") };

        const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 4, nullptr);

        suite.expect(CallsMatch(fake.calls, { { kBase, 2 * kPage }, { kBase + 2 * kPage, 2 * kPage } }),
            L"page reader: the failing block is the second of exactly two Read calls");
        suite.expect(result.channelFailed && result.failure == "disk gone" && result.portCalls == 2,
            L"page reader: a later Failed still reports its own failure text");
        ExpectFullyValid(suite, L"page reader: failed-later page 0", result.pages[0], kBase,
            Slice(okData, 0, static_cast<std::size_t>(kPage)));
        ExpectFullyValid(suite, L"page reader: failed-later page 1", result.pages[1], kBase + kPage,
            Slice(okData, static_cast<std::size_t>(kPage), static_cast<std::size_t>(kPage)));
        ExpectBlankPage(suite, L"page reader: failed-later page 2", result.pages[2], kBase + 2 * kPage,
            PageState::NotAttempted);
        ExpectBlankPage(suite, L"page reader: failed-later page 3", result.pages[3], kBase + 3 * kPage,
            PageState::NotAttempted);
    }
}

// ------------------------------------------------------------
// 六、scratchAreaDirty：本次读到的数据仍然生效，但立即停止后续所有读取。
// ------------------------------------------------------------
void TestScratchAreaDirtyStops(KswordTests::Suite& suite) {
    constexpr std::uint64_t kBase = 0x50000ULL;
    const Bytes okData = MakePattern(0xBB, static_cast<std::size_t>(2 * kPage));

    FakeMemoryIoPort fake;
    fake.limits.maxReadBytes = 2 * kPage; // 两页一块，共两块。
    IoReadResult dirtyOk = MakeOk(okData);
    dirtyOk.scratchAreaDirty = true;
    fake.script = { dirtyOk };

    const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 4, nullptr);

    suite.expect(CallsMatch(fake.calls, { { kBase, 2 * kPage } }),
        L"page reader: a dirty scratch area on the first block prevents the second block's Read");
    suite.expect(result.scratchAreaDirty && !result.channelFailed && result.portCalls == 1,
        L"page reader: scratchAreaDirty is reported without being treated as a channel failure");

    ExpectFullyValid(suite, L"page reader: scratch-dirty page 0", result.pages[0], kBase,
        Slice(okData, 0, static_cast<std::size_t>(kPage)));
    ExpectFullyValid(suite, L"page reader: scratch-dirty page 1", result.pages[1], kBase + kPage,
        Slice(okData, static_cast<std::size_t>(kPage), static_cast<std::size_t>(kPage)));
    ExpectBlankPage(suite, L"page reader: scratch-dirty page 2", result.pages[2], kBase + 2 * kPage,
        PageState::NotAttempted);
    ExpectBlankPage(suite, L"page reader: scratch-dirty page 3", result.pages[3], kBase + 3 * kPage,
        PageState::NotAttempted);
}

// ------------------------------------------------------------
// 六之二、真实 DDMA 端口的映射形状：暂存扇区没能还原时返回 Failed + scratchAreaDirty
// （WorkbenchIoMapping.cpp 的 MapFacadeReadOutcome）。上一版只测了"Ok + 脏"，漏掉了这条
// 真实路径：Failed 分支没把脏标记带出来，调用方的 DDMA 闩锁因此永远不会置位。
// ------------------------------------------------------------
void TestScratchAreaDirtyViaFailedStatus(KswordTests::Suite& suite) {
    constexpr std::uint64_t kBase = 0x58000ULL;

    FakeMemoryIoPort fake;
    fake.limits.maxReadBytes = 2 * kPage; // 两页一块，共两块。
    IoReadResult dirtyFailed = MakeFailed("scratch sector not restored");
    dirtyFailed.scratchAreaDirty = true;
    fake.script = { dirtyFailed };

    const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 4, nullptr);

    suite.expect(result.channelFailed && result.failure == "scratch sector not restored",
        L"page reader: a Failed read is still reported as a channel failure with its text");
    suite.expect(result.scratchAreaDirty,
        L"page reader: the dirty-scratch flag survives the Failed branch (real DDMA port shape)");
    suite.expect(result.portCalls == 1 && CallsMatch(fake.calls, { { kBase, 2 * kPage } }),
        L"page reader: nothing else is read after a dirty Failed result");
    ExpectBlankPage(suite, L"page reader: dirty-failed page 0", result.pages[0], kBase, PageState::NotAttempted);
    ExpectBlankPage(suite, L"page reader: dirty-failed page 3", result.pages[3], kBase + 3 * kPage,
        PageState::NotAttempted);

    // 对照：不脏的 Failed 不得凭空带出脏标记。
    FakeMemoryIoPort cleanFake;
    cleanFake.limits.maxReadBytes = 2 * kPage;
    cleanFake.script = { MakeFailed("channel offline") };
    const PageReadResult clean = ReadPages(cleanFake, MakeSession(), kBase, 4, nullptr);
    suite.expect(clean.channelFailed && !clean.scratchAreaDirty,
        L"page reader: a clean Failed result does not report a dirty scratch area");
}

// ------------------------------------------------------------
// 七、取消：预先置位与"两次调用之间置位"各一例，都在下一次 Read 之前被拦住。
// ------------------------------------------------------------
void TestCancellation(KswordTests::Suite& suite) {
    // 场景 A：调用前取消标志已经置位，一次 Read 都不会发起。
    {
        constexpr std::uint64_t kBase = 0x60000ULL;
        std::atomic<bool> cancelled{ true };
        FakeMemoryIoPort fake;
        fake.limits.maxReadBytes = 0;

        const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 3, &cancelled);

        suite.expect(fake.calls.empty() && result.portCalls == 0,
            L"page reader: a pre-armed cancel flag issues zero Read calls");
        suite.expect(result.cancelled && !result.channelFailed, L"page reader: a pre-armed cancel reports cancelled");
        for (std::uint64_t page = 0; page < 3; ++page) {
            ExpectBlankPage(suite, L"page reader: pre-cancelled page " + std::to_wstring(page),
                result.pages[static_cast<std::size_t>(page)], kBase + page * kPage, PageState::NotAttempted);
        }
    }

    // 场景 B：第一块读完之后才置位（假端口在第 1 次调用返回前帮我们"从另一个
    // 线程"把标志翻过去），第二块应当完全没有发起 Read。
    {
        constexpr std::uint64_t kBase = 0x70000ULL;
        const Bytes okData = MakePattern(0xCC, static_cast<std::size_t>(2 * kPage));
        std::atomic<bool> cancelled{ false };

        FakeMemoryIoPort fake;
        fake.limits.maxReadBytes = 2 * kPage;
        fake.script = { MakeOk(okData) };
        fake.cancelToArmAfterCall = &cancelled;
        fake.cancelArmIndex = 1;

        const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 4, &cancelled);

        suite.expect(CallsMatch(fake.calls, { { kBase, 2 * kPage } }),
            L"page reader: a mid-run cancel still lets the already-started block's data land");
        suite.expect(result.cancelled && !result.channelFailed && result.portCalls == 1,
            L"page reader: the second block is never attempted once the flag flips");
        ExpectFullyValid(suite, L"page reader: mid-cancel page 0", result.pages[0], kBase,
            Slice(okData, 0, static_cast<std::size_t>(kPage)));
        ExpectFullyValid(suite, L"page reader: mid-cancel page 1", result.pages[1], kBase + kPage,
            Slice(okData, static_cast<std::size_t>(kPage), static_cast<std::size_t>(kPage)));
        ExpectBlankPage(suite, L"page reader: mid-cancel page 2", result.pages[2], kBase + 2 * kPage,
            PageState::NotAttempted);
        ExpectBlankPage(suite, L"page reader: mid-cancel page 3", result.pages[3], kBase + 3 * kPage,
            PageState::NotAttempted);
    }
}

// ------------------------------------------------------------
// 八、Limits 切块边界：上限 -1 / 恰好 / +1 页，以及 maxReadBytes=0 与
// "上限不足一页"两种特殊配置。
// ------------------------------------------------------------
void TestLimitBoundaries(KswordTests::Suite& suite) {
    constexpr std::uint64_t kLimitPages = 3;
    const std::uint64_t kLimitBytes = kLimitPages * kPage;

    // 上限 -1 页：请求页数比上限少一页，整段仍是一块。
    {
        constexpr std::uint64_t kBase = 0x80000ULL;
        FakeMemoryIoPort fake;
        fake.limits.maxReadBytes = kLimitBytes;
        fake.script = { MakeOk(MakePattern(0x01, static_cast<std::size_t>(2 * kPage))) };
        const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 2, nullptr);
        suite.expect(CallsMatch(fake.calls, { { kBase, 2 * kPage } }) && result.portCalls == 1,
            L"page reader: limit-1 pages still fit in a single block");
    }

    // 恰好等于上限：仍是一块。
    {
        constexpr std::uint64_t kBase = 0x90000ULL;
        FakeMemoryIoPort fake;
        fake.limits.maxReadBytes = kLimitBytes;
        fake.script = { MakeOk(MakePattern(0x02, static_cast<std::size_t>(3 * kPage))) };
        const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 3, nullptr);
        suite.expect(CallsMatch(fake.calls, { { kBase, 3 * kPage } }) && result.portCalls == 1,
            L"page reader: exactly the limit is still one block");
    }

    // 上限 +1 页：切成两块，第二块只剩一页。
    {
        constexpr std::uint64_t kBase = 0xA0000ULL;
        FakeMemoryIoPort fake;
        fake.limits.maxReadBytes = kLimitBytes;
        fake.script = {
            MakeOk(MakePattern(0x03, static_cast<std::size_t>(3 * kPage))),
            MakeOk(MakePattern(0x04, static_cast<std::size_t>(kPage))),
        };
        const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 4, nullptr);
        suite.expect(CallsMatch(fake.calls, { { kBase, 3 * kPage }, { kBase + 3 * kPage, kPage } })
            && result.portCalls == 2,
            L"page reader: limit+1 pages split into a full block and a one-page remainder");
    }

    // maxReadBytes=0：同样四页，不限时整段仍是一块。
    {
        constexpr std::uint64_t kBase = 0xB0000ULL;
        FakeMemoryIoPort fake;
        fake.limits.maxReadBytes = 0;
        fake.script = { MakeOk(MakePattern(0x05, static_cast<std::size_t>(4 * kPage))) };
        const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 4, nullptr);
        suite.expect(CallsMatch(fake.calls, { { kBase, 4 * kPage } }) && result.portCalls == 1,
            L"page reader: maxReadBytes=0 means no limit regardless of how many pages are requested");
    }

    // 上限不足一页：每块至少 1 页，退化成逐页请求。
    {
        constexpr std::uint64_t kBase = 0x110000ULL;
        FakeMemoryIoPort fake;
        fake.limits.maxReadBytes = 100; // 远小于一页。
        fake.script = {
            MakeOk(MakePattern(0x06, static_cast<std::size_t>(kPage))),
            MakeOk(MakePattern(0x07, static_cast<std::size_t>(kPage))),
        };
        const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 2, nullptr);
        suite.expect(CallsMatch(fake.calls, { { kBase, kPage }, { kBase + kPage, kPage } })
            && result.portCalls == 2,
            L"page reader: a limit below one page still requests at least one full page per call");
    }
}

// ------------------------------------------------------------
// 九、地址空间末端：最后一页恰好能表示则正常处理，多一页就回绕，必须整体拒绝。
// ------------------------------------------------------------
void TestAddressSpaceEnd(KswordTests::Suite& suite) {
    constexpr std::uint64_t kMaxAddress = (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t kLastPageStart = kMaxAddress - (kPage - 1);

    // 恰好是地址空间最后一页：不溢出，正常处理。
    {
        FakeMemoryIoPort fake;
        fake.limits.maxReadBytes = 0;
        fake.script = { MakeOk(MakePattern(0xEE, static_cast<std::size_t>(kPage))) };
        const PageReadResult result = ReadPages(fake, MakeSession(), kLastPageStart, 1, nullptr);
        suite.expect(CallsMatch(fake.calls, { { kLastPageStart, kPage } }) && !result.channelFailed,
            L"page reader: the very last page of the address space is read normally");
        suite.expect(result.pages.size() == 1 && result.pages[0].pageStart == kLastPageStart
            && result.pages[0].state == PageState::Valid,
            L"page reader: the last page's record carries the correct page start");
    }

    // 多要一页：末尾地址会回绕过 2^64，必须整体拒绝，一次 Read 都不发起。
    {
        FakeMemoryIoPort fake;
        fake.limits.maxReadBytes = 0;
        const PageReadResult result = ReadPages(fake, MakeSession(), kLastPageStart, 2, nullptr);
        suite.expect(fake.calls.empty() && result.portCalls == 0,
            L"page reader: an overflowing range never issues a single Read call");
        suite.expect(result.channelFailed && result.pages.empty(),
            L"page reader: an overflowing range is reported as a channel failure with no page records");
    }
}

// ------------------------------------------------------------
// 十、精确调用序列：Partial(跨 1 页又多 50 字节) -> 批量失败 -> 单页核验，
// 与前面几个场景用不同的切片长度，独立钉死一遍"调用序列=手算结果"。
// ------------------------------------------------------------
void TestExactCallSequenceIsHandCalculated(KswordTests::Suite& suite) {
    constexpr std::uint64_t kBase = 0x120000ULL;
    const Bytes partialData = MakePattern(0x05, static_cast<std::size_t>(kPage) + 50);
    const Bytes okData = MakePattern(0x99, static_cast<std::size_t>(kPage));

    FakeMemoryIoPort fake;
    fake.limits.maxReadBytes = 0;
    fake.script = { MakePartial(partialData), MakeUnreadable("bulk unknown hole"),
        MakeUnreadable("page 2 hole"), MakeOk(okData) };

    const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 4, nullptr);

    suite.expect(CallsMatch(fake.calls,
        { { kBase, 4 * kPage }, { kBase + 2 * kPage, 2 * kPage },
            { kBase + 2 * kPage, kPage }, { kBase + 3 * kPage, kPage } }),
        L"page reader: a partial prefix remains valid when a later bulk failure requires two single-page checks");
    suite.expect(result.portCalls == 4, L"page reader: portCalls equals the number of calls just asserted");

    ExpectFullyValid(suite, L"page reader: exact-sequence page 0", result.pages[0], kBase,
        Slice(partialData, 0, static_cast<std::size_t>(kPage)));
    ExpectPartiallyValid(suite, L"page reader: exact-sequence page 1", result.pages[1], kBase + kPage, 50,
        Slice(partialData, static_cast<std::size_t>(kPage), 50));
    ExpectBlankPage(suite, L"page reader: exact-sequence page 2", result.pages[2], kBase + 2 * kPage, PageState::Unreadable);
    ExpectFullyValid(suite, L"page reader: exact-sequence page 3", result.pages[3], kBase + 3 * kPage, okData);
}

// ------------------------------------------------------------
// 十一、空请求与会话无关：pageCount 为 0 时什么都不做；readModifyWriteWindow
// 在多次调用里按逻辑或累加。
// ------------------------------------------------------------
void TestEmptyRequestAndWindowAggregation(KswordTests::Suite& suite) {
    FakeMemoryIoPort fake;
    fake.limits.maxReadBytes = 0;
    const PageReadResult empty = ReadPages(fake, MakeSession(), 0x1000ULL, 0, nullptr);
    suite.expect(fake.calls.empty() && empty.pages.empty() && empty.portCalls == 0 && !empty.channelFailed,
        L"page reader: pageCount==0 touches the port zero times and reports nothing happened");

    constexpr std::uint64_t kBase = 0x130000ULL;
    FakeMemoryIoPort rmwFake;
    rmwFake.limits.maxReadBytes = kPage; // 每块一页，共两次调用。
    IoReadResult firstHit = MakeOk(MakePattern(0x11, static_cast<std::size_t>(kPage)));
    firstHit.readModifyWriteWindow = true;
    IoReadResult secondHit = MakeOk(MakePattern(0x12, static_cast<std::size_t>(kPage)));
    rmwFake.script = { firstHit, secondHit };
    const PageReadResult rmwResult = ReadPages(rmwFake, MakeSession(), kBase, 2, nullptr);
    suite.expect(rmwResult.readModifyWriteWindow,
        L"page reader: readModifyWriteWindow aggregates true even if only one of several calls reports it");
}

// ------------------------------------------------------------
// 十二（P-1）：注记聚合——Ok/Partial 结果里的非空 failure 文本按出现顺序去重
// 并入 note，最多保留 3 条；Unreadable 的非空 failure 只取第一个，进
// unreadableReason，后续同类结果不覆盖它。七页一块（每块恰好一页，便于逐页
// 精确编排），覆盖：同一段文本重复出现必须去重、第 4 条不同文本必须被上限
// 丢弃、两次 Unreadable 只认第一次。
// ------------------------------------------------------------
void TestNoteAggregationDedupAndCap(KswordTests::Suite& suite) {
    constexpr std::uint64_t kBase = 0x140000ULL;
    FakeMemoryIoPort fake;
    fake.limits.maxReadBytes = kPage; // 每块恰好一页：一页对应一次 Read、一个可编排的注记。

    IoReadResult page0 = MakeOk(MakePattern(0x01, static_cast<std::size_t>(kPage)));
    page0.failure = "note-A";
    IoReadResult page1 = MakePartial(MakePattern(0x02, 4));
    page1.failure = "note-B";
    IoReadResult page2 = MakeOk(MakePattern(0x03, static_cast<std::size_t>(kPage)));
    page2.failure = "note-A"; // 与 page0 完全相同的文本：必须被去重，不重复计入。
    IoReadResult page3 = MakeOk(MakePattern(0x04, static_cast<std::size_t>(kPage)));
    page3.failure = "note-C"; // 第三条不同文本，凑满上限 3 条。
    IoReadResult page4 = MakeOk(MakePattern(0x05, static_cast<std::size_t>(kPage)));
    page4.failure = "note-D"; // 第四条不同文本：必须被"最多 3 条"的上限丢弃。
    const IoReadResult page5 = MakeUnreadable("unreadable-reason-1");
    const IoReadResult page6 = MakeUnreadable("unreadable-reason-2"); // 第二次 Unreadable，不得覆盖第一次。

    fake.script = { page0, page1, page2, page3, page4, page5, page6 };

    const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 7, nullptr);

    suite.expect(result.portCalls == 7 && !result.channelFailed,
        L"page reader: seven one-page blocks each issue exactly one Read and never stop early");
    suite.expect(result.note == "note-A；note-B；note-C",
        L"page reader: note dedupes a repeated entry and keeps only the first three distinct ones, in order");
    suite.expect(result.unreadableReason == "unreadable-reason-1",
        L"page reader: unreadableReason keeps only the first Unreadable result's failure text");
}

// ------------------------------------------------------------
// 十三（P-1）：Failed 的 failure 只进顶层 result.failure，绝不混入 note——
// 先让第一块带着一条注记成功，第二块 Failed 整体停止，note 必须仍然只有
// 第一块那一条，不能被 Failed 的文本污染或覆盖。
// ------------------------------------------------------------
void TestFailedDoesNotPolluteNote(KswordTests::Suite& suite) {
    constexpr std::uint64_t kBase = 0x150000ULL;
    FakeMemoryIoPort fake;
    fake.limits.maxReadBytes = kPage;

    IoReadResult page0 = MakeOk(MakePattern(0x06, static_cast<std::size_t>(kPage)));
    page0.failure = "note-E";
    const IoReadResult page1 = MakeFailed("channel exploded");

    fake.script = { page0, page1 };

    const PageReadResult result = ReadPages(fake, MakeSession(), kBase, 2, nullptr);

    suite.expect(result.channelFailed && result.failure == "channel exploded",
        L"page reader: a later Failed still reports its own failure text at the top level");
    suite.expect(result.note == "note-E",
        L"page reader: Failed's failure text never folds into note; the earlier Ok note is left untouched");
}

// This oracle models the measured Windows API contract, not the reader's flow:
// any hole anywhere in a request returns no data; readable individual pages do.
class AllOrNothingPages final : public IMemoryIoPort {
public:
    static constexpr std::uint64_t Base = 0x160000;
    std::vector<bool> readable;
    std::vector<ReadCall> calls;
    bool wrote = false;
    IoLimits Limits(const MemoryTargetSession&) const override { return {}; }
    IoReadResult Read(const MemoryTargetSession&, std::uint64_t address, std::uint64_t length) override {
        calls.push_back({address, length});
        if (address < Base || (address - Base) % kPage || length % kPage || !length)
            return MakeFailed("invalid request in all-or-nothing fixture");
        const auto start = (address - Base) / kPage;
        const auto count = length / kPage;
        if (start >= readable.size() || count > readable.size() - start) return MakeFailed("range outside fixture");
        for (std::uint64_t i = 0; i < count; ++i)
            if (!readable[static_cast<std::size_t>(start + i)]) return MakeUnreadable("RPM whole-range failure");
        Bytes bytes(static_cast<std::size_t>(length));
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(7 + start + i / kPage);
        return MakeOk(std::move(bytes));
    }
    IoWriteResult Write(const MemoryTargetSession&, std::uint64_t, const Bytes&, bool) override {
        wrote = true; return {};
    }
};

void TestActualRpmAllOrNothingModel(KswordTests::Suite& suite) {
    const std::vector<std::vector<bool>> layouts = {{true, false, true}, {true, true, false},
        {false, true, true}, {false, false, false}, {false}, {true, true, true}};
    std::size_t scenario = 0;
    for (const auto& layout : layouts) {
        AllOrNothingPages port;
        port.readable = layout;
        const auto result = ReadPages(port, MakeSession(), port.Base, layout.size(), nullptr);
        const bool holes = std::find(layout.begin(), layout.end(), false) != layout.end();
        const auto expectedCalls = holes && layout.size() > 1 ? layout.size() + 1 : 1;
        const auto label = L"RPM oracle scenario " + std::to_wstring(++scenario);
        suite.expect(!result.channelFailed && !result.cancelled && !port.wrote && result.pages.size() == layout.size(),
            (label + L": complete read-only result without channel failure").c_str());
        suite.expect(port.calls.size() == expectedCalls && result.portCalls == expectedCalls,
            (label + L": healthy bulk stays one call, a hole costs at most one bulk plus one call per page").c_str());
        suite.expect(port.calls.front().address == port.Base && port.calls.front().length == layout.size() * kPage,
            (label + L": first request retains full healthy-range throughput").c_str());
        if (expectedCalls > 1 && port.calls.size() == expectedCalls)
            for (std::size_t i = 0; i < layout.size(); ++i)
                suite.expect(port.calls[i + 1].address == port.Base + i * kPage && port.calls[i + 1].length == kPage,
                    (label + L": fallback verifies each page exactly once, including readable leading pages").c_str());
        for (std::size_t i = 0; i < layout.size(); ++i) {
            if (layout[i]) ExpectFullyValid(suite, label, result.pages[i], port.Base + i * kPage,
                Bytes(static_cast<std::size_t>(kPage), static_cast<std::uint8_t>(7 + i)));
            else ExpectBlankPage(suite, label, result.pages[i], port.Base + i * kPage, PageState::Unreadable);
        }
    }
}

void TestFallbackBlockResetAndAnnotations(KswordTests::Suite& suite) {
    constexpr std::uint64_t base = 0x180000;
    const auto page = MakePattern(0x22, static_cast<std::size_t>(kPage));
    FakeMemoryIoPort port;
    port.limits.maxReadBytes = 2 * kPage;
    port.script = {MakeUnreadable("ambiguous bulk"), MakeOk(page), MakeUnreadable("known hole"),
        MakeOk(MakePattern(0x33, static_cast<std::size_t>(2 * kPage)))};
    auto result = ReadPages(port, MakeSession(), base, 4, nullptr);
    suite.expect(CallsMatch(port.calls, {{base, 2 * kPage}, {base, kPage}, {base + kPage, kPage},
        {base + 2 * kPage, 2 * kPage}}), L"fallback: the next block returns to bulk mode without re-scanning the failed block");
    suite.expect(result.pages[0].state == PageState::Valid && result.pages[1].state == PageState::Unreadable
        && result.pages[2].state == PageState::Valid && result.pages[3].state == PageState::Valid
        && result.unreadableReason == "known hole", L"fallback: only verified pages carry unreadable state/reason");
    FakeMemoryIoPort annotated;
    auto bulk = MakeUnreadable("transient bulk failure"); bulk.readModifyWriteWindow = true;
    auto partial = MakePartial(MakePattern(0x44, 17)); partial.failure = "note-A";
    auto full = MakeOk(page); full.failure = "note-A";
    auto last = MakeOk(page); last.failure = "note-B";
    annotated.script = {bulk, partial, full, last};
    result = ReadPages(annotated, MakeSession(), base, 3, nullptr);
    ExpectPartiallyValid(suite, L"fallback: partial single page", result.pages[0], base, 17, partial.data);
    ExpectFullyValid(suite, L"fallback: first recovered neighbor", result.pages[1], base + kPage, page);
    ExpectFullyValid(suite, L"fallback: second recovered neighbor", result.pages[2], base + 2 * kPage, page);
    suite.expect(result.portCalls == 4 && result.readModifyWriteWindow && !result.channelFailed
        && result.note == "note-A；note-B" && result.unreadableReason.empty(),
        L"fallback: single-page partials preserve notes/dedup/window flags without claiming an ambiguous bulk hole");
}

void TestFallbackStopsBeforeUnsafeProbes(KswordTests::Suite& suite) {
    constexpr std::uint64_t base = 0x190000;
    for (int scenario = 0; scenario < 7; ++scenario) {
        std::atomic<bool> cancel{false};
        FakeMemoryIoPort port;
        auto bulk = MakeUnreadable("bulk unavailable"); bulk.readModifyWriteWindow = true;
        auto probe = MakeOk(MakePattern(0x51, static_cast<std::size_t>(kPage)));
        if (scenario == 0) probe = MakeFailed("probe channel failure");
        if (scenario == 1) bulk.scratchAreaDirty = true;
        if (scenario == 2) { probe = MakeUnreadable("dirty single page"); probe.scratchAreaDirty = true; }
        if (scenario == 3 || scenario == 4) {
            port.cancelToArmAfterCall = &cancel; port.cancelArmIndex = scenario == 3 ? 1 : 2;
        }
        if (scenario == 5) probe.scratchAreaDirty = true;
        if (scenario == 6) { probe = MakePartial(MakePattern(0x52, 5)); probe.scratchAreaDirty = true; }
        port.script = {bulk, probe};
        const auto result = ReadPages(port, MakeSession(), base, 3, &cancel);
        const auto label = L"fallback stop scenario " + std::to_wstring(scenario);
        const auto expectedCalls = scenario == 1 || scenario == 3 ? 1U : 2U;
        suite.expect(result.portCalls == expectedCalls && port.calls.size() == expectedCalls,
            (label + L": no callback after cancellation, channel failure or scratch fault").c_str());
        suite.expect(result.channelFailed == (scenario == 0) && result.cancelled == (scenario == 3 || scenario == 4)
            && result.scratchAreaDirty == (scenario == 1 || scenario == 2 || scenario == 5 || scenario == 6)
            && result.readModifyWriteWindow, (label + L": failure/dirty/cancel/window flags retain their separate meanings").c_str());
        if (scenario == 2) ExpectBlankPage(suite, label, result.pages[0], base, PageState::Unreadable);
        else if (scenario == 4 || scenario == 5) ExpectFullyValid(suite, label, result.pages[0], base, probe.data);
        else if (scenario == 6) ExpectPartiallyValid(suite, label, result.pages[0], base, 5, probe.data);
        else ExpectBlankPage(suite, label, result.pages[0], base, PageState::NotAttempted);
        for (std::size_t i = 1; i < result.pages.size(); ++i)
            ExpectBlankPage(suite, label, result.pages[i], base + i * kPage, PageState::NotAttempted);
        if (scenario == 0) suite.expect(result.failure == "probe channel failure" && result.note.empty(),
            L"fallback: Failed stops immediately and remains separate from successful-read notes");
    }
}

} // namespace

int RunMemwbPageReaderTests() {
    KswordTests::Suite suite(L"MEMWB page reader");
    TestFullOk(suite);
    TestPartialContinuesFromNextPage(suite);
    TestUnreadableThenContinue(suite);
    TestMultipleHoles(suite);
    TestFailedStopsEverything(suite);
    TestScratchAreaDirtyStops(suite);
    TestScratchAreaDirtyViaFailedStatus(suite);
    TestCancellation(suite);
    TestLimitBoundaries(suite);
    TestAddressSpaceEnd(suite);
    TestExactCallSequenceIsHandCalculated(suite);
    TestEmptyRequestAndWindowAggregation(suite);
    TestNoteAggregationDedupAndCap(suite);
    TestFailedDoesNotPolluteNote(suite);
    TestActualRpmAllOrNothingModel(suite);
    TestFallbackBlockResetAndAnnotations(suite);
    TestFallbackStopsBeforeUnsafeProbes(suite);
    suite.report();
    return suite.failures();
}
