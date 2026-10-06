// memwb_ui_tests.CachedRange.cpp
// 作用：Phase 3 WP-0 的离屏验证（第二组）——HexCanvas::copyCachedRange / copyCachedRangeWithMask：
//   1) 严格版：整段"已缓存且读到了"才返回，页内、跨页、整页、单字节、空间两端都有手算期望值；
//   2) 未加载 / 在途 / 整页不可读 / 部分读里没读到的字节：严格版整体失败，绝不补零、不返回一半；
//   3) 掩码版：已读完但读不到的字节以 0 + 掩码 0 返回（整页不可读、部分读两种），未落定的仍整体失败；
//   4) 区间非法（反向、越出地址空间）、覆盖整个 2^64 的超长区间不溢出不分配、空间最高地址处不溢出；
//   5) 返回的是页缓存的原始值而不是"所见值"：暂存补丁不叠加；补丁所在页被清出缓存后不算已加载；
//   6) 换来源代次（refresh）后旧缓存作废，陈旧回填不会让它复活；
//   7) 与 cellStateAt 逐字节对拍：同一块混合状态画布上，3000 个确定性随机区间（含越界与跨页）的结果
//      逐字节等于按 cellStateAt 推出的期望，且各类结果都真的出现过（自证不是空转）；
//   8) 无副作用（不发 contentChanged、不换代次）；与 MemoryDiffOverlay::RefreshBaseline 的数据格式衔接；
//   9) 1 MiB（缓存容量）整块拷出的内容与耗时。
// 期望值全部手算写死（MakePattern 的第 i 字节 = (i*37+11) 取低 8 位，不为 0）。

#include "memwb_ui_signals.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QSignalSpy>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using Bytes = std::vector<std::uint8_t>;

        // kBase：混合状态画布的地址空间起点（页对齐）。kPage：页大小。kPages：地址空间页数。
        constexpr std::uint64_t kBase = 0x100000ULL;
        constexpr std::uint64_t kPage = 4096ULL;
        constexpr std::uint64_t kPages = 16ULL;

        // PageAt：混合画布第 index 页的起始地址。
        std::uint64_t PageAt(std::uint64_t index)
        {
            return kBase + index * kPage;
        }

        // AppendRun：往字节串末尾追加 count 个相同的值（手写期望掩码时用）。
        // 传入：目标字节串、个数、值；传出：无（直接修改目标）。
        void AppendRun(Bytes* target, int count, std::uint8_t value)
        {
            for (int index = 0; index < count; ++index)
            {
                target->push_back(value);
            }
        }

        // MakeRange：构造只含"起点 + 页数"的页范围。
        ks::ui::HexFetchRange MakeRange(std::uint64_t pageStart, std::uint64_t pageCount)
        {
            ks::ui::HexFetchRange range;
            range.firstPageStart = pageStart;
            range.pageCount = pageCount;
            range.distancePages = 0;
            return range;
        }

        // MakeMixedCanvas：构造 16 页的混合状态画布（无叠加层，页全部由测试手动投递）：
        //   页 0、1、4、6 有效（内容 = MakePattern(4096)）；
        //   页 2、7 整页不可读；
        //   页 3 部分读：页内偏移 16..31 的 16 个字节没读到（掩码 0，但页缓冲里这些位置放的是非零垃圾）；
        //   页 5、8..15 从未加载（也不在途）。
        std::unique_ptr<AsyncCanvas> MakeMixedCanvas()
        {
            auto holder = MakeAsyncCanvas(kBase, PageAt(kPages) - 1ULL, false, false);
            HexCanvas& canvas = *holder->canvas;
            const std::uint64_t revision = canvas.sourceRevision();
            CHECK(DeliverPage(canvas, PageAt(0)) == HexCanvas::PageResult::Accepted);
            CHECK(DeliverPage(canvas, PageAt(1)) == HexCanvas::PageResult::Accepted);
            CHECK(canvas.deliverUnreadable(MakeRange(PageAt(2), 1), revision) == HexCanvas::PageResult::Accepted);

            // 页 3 的掩码：全 1，偏移 16..31 为 0。
            QByteArray partialMask(4096, '\x01');
            for (int offset = 16; offset <= 31; ++offset)
            {
                partialMask[offset] = '\x00';
            }
            CHECK(canvas.deliverPage(PageAt(3), MakePattern(4096), partialMask, revision) == HexCanvas::PageResult::Accepted);
            CHECK(DeliverPage(canvas, PageAt(4)) == HexCanvas::PageResult::Accepted);
            CHECK(DeliverPage(canvas, PageAt(6)) == HexCanvas::PageResult::Accepted);
            CHECK(canvas.deliverUnreadable(MakeRange(PageAt(7), 1), revision) == HexCanvas::PageResult::Accepted);
            Flush();
            return holder;
        }

        // 严格版：页内、跨页、整页、单字节、空间起点，期望值手算写死。
        void TestStrictValidRanges()
        {
            ApplyTheme(false);
            auto holder = MakeMixedCanvas();
            HexCanvas& canvas = *holder->canvas;

            // 页 0 开头 8 字节：(i*37+11) & 0xFF，i = 0..7。
            const std::optional<Bytes> head = canvas.copyCachedRange(kBase, kBase + 7);
            CHECK(head.has_value());
            CHECK(head.has_value() && *head == Bytes({ 0x0B, 0x30, 0x55, 0x7A, 0x9F, 0xC4, 0xE9, 0x0E }));

            // 单字节：页首与页尾（i = 4095 时 (4095*37+11) & 0xFF = 0xE6）。
            const std::optional<Bytes> first = canvas.copyCachedRange(kBase, kBase);
            CHECK(first.has_value() && *first == Bytes({ 0x0B }));
            const std::optional<Bytes> lastOfPage = canvas.copyCachedRange(PageAt(1) - 1, PageAt(1) - 1);
            CHECK(lastOfPage.has_value() && *lastOfPage == Bytes({ 0xE6 }));

            // 跨页 0 与页 1 的边界：页 0 的 4094、4095 号字节，页 1 的 0、1 号字节。
            const std::optional<Bytes> crossing = canvas.copyCachedRange(PageAt(1) - 2, PageAt(1) + 1);
            CHECK(crossing.has_value() && *crossing == Bytes({ 0xC1, 0xE6, 0x0B, 0x30 }));

            // 页 0 与页 1 整两页：8192 字节，前后两半都是 MakePattern(4096)。
            const std::optional<Bytes> twoPages = canvas.copyCachedRange(PageAt(0), PageAt(2) - 1);
            CHECK(twoPages.has_value());
            if (twoPages.has_value())
            {
                const QByteArray pattern = MakePattern(4096);
                CHECK(twoPages->size() == 8192);
                CHECK(twoPages->size() == 8192 && std::memcmp(twoPages->data(), pattern.constData(), 4096) == 0);
                CHECK(twoPages->size() == 8192 && std::memcmp(twoPages->data() + 4096, pattern.constData(), 4096) == 0);
            }

            // 页 4（孤立的有效页，前后都不是有效页）内部：4 号字节起 4 个字节 = 0x9F, 0xC4, 0xE9, 0x0E。
            const std::optional<Bytes> isolated = canvas.copyCachedRange(PageAt(4) + 4, PageAt(4) + 7);
            CHECK(isolated.has_value() && *isolated == Bytes({ 0x9F, 0xC4, 0xE9, 0x0E }));

            // 部分读页 3 里"读到了"的那部分：偏移 0..3 与 32..35（32*37+11 = 1195, 取低 8 位 0xAB）。
            const std::optional<Bytes> partialHead = canvas.copyCachedRange(PageAt(3), PageAt(3) + 3);
            CHECK(partialHead.has_value() && *partialHead == Bytes({ 0x0B, 0x30, 0x55, 0x7A }));
            const std::optional<Bytes> partialTail = canvas.copyCachedRange(PageAt(3) + 32, PageAt(3) + 33);
            CHECK(partialTail.has_value() && *partialTail == Bytes({ 0xAB, 0xD0 }));

            // 每次调用返回相同结果（无状态）。
            const std::optional<Bytes> again = canvas.copyCachedRange(kBase, kBase + 7);
            CHECK(again.has_value() && head.has_value() && *again == *head);
        }

        // 严格版的拒绝路径：未加载、不可读、部分读里没读到的字节，哪怕只占一个字节也整体失败。
        void TestStrictRejections()
        {
            ApplyTheme(false);
            auto holder = MakeMixedCanvas();
            HexCanvas& canvas = *holder->canvas;

            // 前置：确认这几页确实处于预期的状态（否则下面的断言没有意义）。
            CHECK(canvas.cellStateAt(PageAt(5)).byteState == HexCanvas::ByteState::NotLoaded);
            CHECK(canvas.cellStateAt(PageAt(2)).byteState == HexCanvas::ByteState::Unreadable);
            CHECK(canvas.cellStateAt(PageAt(3) + 16).byteState == HexCanvas::ByteState::Unreadable);
            CHECK(canvas.cellStateAt(PageAt(3) + 15).byteState == HexCanvas::ByteState::Valid);
            CHECK(canvas.cellStateAt(PageAt(3) + 32).byteState == HexCanvas::ByteState::Valid);

            // 整页未加载 / 整页不可读。
            CHECK(!canvas.copyCachedRange(PageAt(5), PageAt(5) + 3).has_value());
            CHECK(!canvas.copyCachedRange(PageAt(2), PageAt(2) + 3).has_value());

            // 有效区间的末尾只多出一个不可读字节（页 1 的最后 96 字节 + 页 2 首字节：页 2 整页不可读）。
            CHECK(!canvas.copyCachedRange(PageAt(1) + 4000, PageAt(2)).has_value());
            // 有效页 4 的末尾紧跟未加载页 5 的第一个字节：整体失败。
            CHECK(!canvas.copyCachedRange(PageAt(4) + 4090, PageAt(5)).has_value());
            // 对照：页 3（部分读）的 4095 号字节是读到了的，与页 4 的首字节拼在一起应当成功；
            // 页 3 的 31 号字节（没读到）单独、或夹在任何区间里都失败。
            CHECK(canvas.copyCachedRange(PageAt(3) + 4095, PageAt(4)).has_value());
            CHECK(!canvas.copyCachedRange(PageAt(3) + 31, PageAt(3) + 31).has_value());
            CHECK(!canvas.copyCachedRange(PageAt(3) + 15, PageAt(3) + 16).has_value());
            CHECK(!canvas.copyCachedRange(PageAt(3), PageAt(3) + 40).has_value());
            CHECK(!canvas.copyCachedRange(PageAt(3) + 31, PageAt(3) + 32).has_value());

            // 部分读区间的边界：15 与 32 两个"读到了"的字节紧邻没读到的区间，各自单独成功。
            CHECK(canvas.copyCachedRange(PageAt(3) + 15, PageAt(3) + 15).has_value());
            CHECK(canvas.copyCachedRange(PageAt(3) + 32, PageAt(3) + 32).has_value());

            // 空间最后一页（未加载）的最后一个字节。
            CHECK(!canvas.copyCachedRange(PageAt(kPages) - 1, PageAt(kPages) - 1).has_value());
        }

        // 区间非法：反向、越出地址空间（起点之前 / 终点之后 / 整个在外面）。
        void TestInvalidRanges()
        {
            ApplyTheme(false);
            auto holder = MakeMixedCanvas();
            HexCanvas& canvas = *holder->canvas;

            // 反向区间（first > last），哪怕两个端点都是有效字节。
            CHECK(!canvas.copyCachedRange(kBase + 5, kBase + 4).has_value());
            CHECK(!canvas.copyCachedRangeWithMask(kBase + 5, kBase + 4).has_value());

            // 起点在空间之前一个字节、终点在空间之后一个字节、整个在外面。
            CHECK(!canvas.copyCachedRange(kBase - 1, kBase + 3).has_value());
            CHECK(!canvas.copyCachedRangeWithMask(kBase - 1, kBase + 3).has_value());
            CHECK(!canvas.copyCachedRange(PageAt(kPages) - 1, PageAt(kPages)).has_value());
            CHECK(!canvas.copyCachedRangeWithMask(PageAt(kPages) - 1, PageAt(kPages)).has_value());
            CHECK(!canvas.copyCachedRange(0, 7).has_value());
            CHECK(!canvas.copyCachedRangeWithMask(0, 7).has_value());

            // 无地址空间：任何区间都是空。
            HexCanvas empty;
            CHECK(!empty.copyCachedRange(0, 0).has_value());
            CHECK(!empty.copyCachedRangeWithMask(0, 0).has_value());
        }

        // 掩码版：整页不可读与部分读都以 0 字节 + 掩码 0 返回，未落定的仍整体失败。
        void TestMaskVersion()
        {
            ApplyTheme(false);
            auto holder = MakeMixedCanvas();
            HexCanvas& canvas = *holder->canvas;

            // 全有效的区间：与严格版字节相同，掩码全 1。
            const std::optional<HexCanvas::CachedRangeCopy> valid = canvas.copyCachedRangeWithMask(PageAt(1) - 2, PageAt(1) + 1);
            CHECK(valid.has_value());
            CHECK(valid.has_value() && valid->bytes == Bytes({ 0xC1, 0xE6, 0x0B, 0x30 }));
            CHECK(valid.has_value() && valid->validMask == Bytes({ 1, 1, 1, 1 }));

            // 整页不可读：4 个 0 字节，掩码全 0；严格版对同一区间失败。
            const std::optional<HexCanvas::CachedRangeCopy> unreadablePage = canvas.copyCachedRangeWithMask(PageAt(2), PageAt(2) + 3);
            CHECK(unreadablePage.has_value());
            CHECK(unreadablePage.has_value() && unreadablePage->bytes == Bytes({ 0, 0, 0, 0 }));
            CHECK(unreadablePage.has_value() && unreadablePage->validMask == Bytes({ 0, 0, 0, 0 }));
            CHECK(!canvas.copyCachedRange(PageAt(2), PageAt(2) + 3).has_value());

            // 部分读：页 3 偏移 8..23，8..15 读到了（(i*37+11) 取低 8 位：0x33 0x58 0x7D 0xA2 0xC7 0xEC 0x11 0x36），
            // 16..23 没读到——页缓冲里这些位置是非零垃圾，返回必须是 0 而不是垃圾。
            const std::optional<HexCanvas::CachedRangeCopy> partial = canvas.copyCachedRangeWithMask(PageAt(3) + 8, PageAt(3) + 23);
            CHECK(partial.has_value());
            CHECK(partial.has_value() && partial->bytes == Bytes({
                0x33, 0x58, 0x7D, 0xA2, 0xC7, 0xEC, 0x11, 0x36, 0, 0, 0, 0, 0, 0, 0, 0 }));
            CHECK(partial.has_value() && partial->validMask == Bytes({
                1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 }));

            // 跨"整页不可读 → 部分读"：页 2 末尾 8 字节（不可读）+ 页 3 偏移 0..39，共 48 字节。
            // 掩码：8 个 0 + 16 个 1 + 16 个 0 + 8 个 1；字节：不可读处为 0。
            const std::optional<HexCanvas::CachedRangeCopy> mixed = canvas.copyCachedRangeWithMask(PageAt(3) - 8, PageAt(3) + 39);
            CHECK(mixed.has_value());
            if (mixed.has_value())
            {
                CHECK(mixed->bytes.size() == 48);
                CHECK(mixed->validMask.size() == 48);
                Bytes expectedMask;
                AppendRun(&expectedMask, 8, 0);
                AppendRun(&expectedMask, 16, 1);
                AppendRun(&expectedMask, 16, 0);
                AppendRun(&expectedMask, 8, 1);
                CHECK(mixed->validMask == expectedMask);
                // 掩码为 1 的位置才有真实字节：页 3 偏移 0..15 = (i*37+11) 低 8 位，偏移 32..39 同理。
                CHECK(mixed->bytes.size() == 48 && mixed->bytes[8] == 0x0B && mixed->bytes[23] == 0x36);
                CHECK(mixed->bytes.size() == 48 && mixed->bytes[40] == 0xAB);
                // 掩码为 0 的位置字节恒为 0。
                for (std::size_t index = 0; index < mixed->bytes.size() && index < mixed->validMask.size(); ++index)
                {
                    CHECK(mixed->validMask[index] != 0 || mixed->bytes[index] == 0);
                }
            }
            CHECK(!canvas.copyCachedRange(PageAt(3) - 8, PageAt(3) + 39).has_value());

            // 未加载：掩码版也整体失败（它只放过"已读完但读不到"，不放过"还没读"）。
            CHECK(!canvas.copyCachedRangeWithMask(PageAt(5), PageAt(5) + 3).has_value());
            CHECK(!canvas.copyCachedRangeWithMask(PageAt(4) + 4090, PageAt(5)).has_value());
            // 不可读页紧挨未加载页：整体失败（页 7 不可读，页 8 未加载）。
            CHECK(!canvas.copyCachedRangeWithMask(PageAt(8) - 4, PageAt(8)).has_value());
            CHECK(canvas.copyCachedRangeWithMask(PageAt(8) - 4, PageAt(8) - 1).has_value());
        }

        // 在途页：严格版与掩码版都视为"未落定"而整体失败。
        void TestPendingPage()
        {
            ApplyTheme(false);
            auto holder = MakeAsyncCanvas(kBase, PageAt(64) - 1ULL, false, false);
            HexCanvas& canvas = *holder->canvas;
            CHECK(DeliverPage(canvas, PageAt(0)) == HexCanvas::PageResult::Accepted);
            CHECK(DeliverPage(canvas, PageAt(1)) == HexCanvas::PageResult::Accepted);
            Flush();

            // 前置：页 2 在预取窗口里、已登记在途但没有回填 = Pending；页 3 没人请求 = NotLoaded。
            CHECK(canvas.cellStateAt(PageAt(2)).byteState == HexCanvas::ByteState::Pending);
            CHECK(canvas.cellStateAt(PageAt(3)).byteState == HexCanvas::ByteState::NotLoaded);

            CHECK(canvas.copyCachedRange(PageAt(0), PageAt(2) - 1).has_value());
            CHECK(!canvas.copyCachedRange(PageAt(2) - 1, PageAt(2)).has_value());
            CHECK(!canvas.copyCachedRangeWithMask(PageAt(2) - 1, PageAt(2)).has_value());
            CHECK(!canvas.copyCachedRange(PageAt(2), PageAt(2) + 3).has_value());
            CHECK(!canvas.copyCachedRangeWithMask(PageAt(2), PageAt(2) + 3).has_value());
            CHECK(!canvas.copyCachedRangeWithMask(PageAt(3), PageAt(3) + 3).has_value());

            // 提供者放弃在途（cancelPages）后页 2 变成未加载，结论不变；真正回填后才可取。
            canvas.cancelPages(MakeRange(PageAt(2), 1));
            CHECK(canvas.cellStateAt(PageAt(2)).byteState == HexCanvas::ByteState::NotLoaded);
            CHECK(!canvas.copyCachedRange(PageAt(2), PageAt(2) + 3).has_value());
            CHECK(DeliverPage(canvas, PageAt(2)) == HexCanvas::PageResult::Accepted);
            const std::optional<Bytes> filled = canvas.copyCachedRange(PageAt(2), PageAt(2) + 3);
            CHECK(filled.has_value() && *filled == Bytes({ 0x0B, 0x30, 0x55, 0x7A }));
        }

        // 超长区间与地址空间最高端：不溢出、不分配、不抛异常。
        void TestHugeRangeAndTopOfAddressSpace()
        {
            ApplyTheme(false);
            HexCanvas canvas;
            RecordingProvider provider;
            canvas.resize(900, 420);
            canvas.show();
            canvas.setAddressSpace(0, UINT64_MAX);
            canvas.setPageProvider(&provider);
            Flush();

            // 覆盖整个 2^64 的区间：长度本身会回绕成 0，必须先按容量拒绝；两个版本都不抛异常、不返回"空但成功"。
            bool threw = false;
            bool strictEngaged = true;
            bool maskEngaged = true;
            QElapsedTimer timer;
            timer.start();
            try
            {
                strictEngaged = canvas.copyCachedRange(0, UINT64_MAX).has_value();
                maskEngaged = canvas.copyCachedRangeWithMask(0, UINT64_MAX).has_value();
            }
            catch (...)
            {
                threw = true;
            }
            CHECK(!threw);
            CHECK(!strictEngaged);
            CHECK(!maskEngaged);
            CHECK(timer.elapsed() < 500);

            // 只比容量多一个字节的区间同样被拒（页缓存默认 256 页 = 1 MiB）。
            CHECK(!canvas.copyCachedRange(0, 256ULL * 4096ULL).has_value());
            CHECK(!canvas.copyCachedRangeWithMask(0, 256ULL * 4096ULL).has_value());

            // 空间最高的页（0xFFFFFFFFFFFFF000）有效：最后 8 个字节取得到，且地址加法在 2^64 处不回绕。
            // 偏移 4088..4095 的字节：(i*37+11) 取低 8 位 = E3 08 2D 52 77 9C C1 E6。
            CHECK(DeliverPage(canvas, 0xFFFFFFFFFFFFF000ULL) == HexCanvas::PageResult::Accepted);
            const std::optional<Bytes> top = canvas.copyCachedRange(UINT64_MAX - 7ULL, UINT64_MAX);
            CHECK(top.has_value() && *top == Bytes({ 0xE3, 0x08, 0x2D, 0x52, 0x77, 0x9C, 0xC1, 0xE6 }));
            const std::optional<HexCanvas::CachedRangeCopy> topMask = canvas.copyCachedRangeWithMask(UINT64_MAX - 7ULL, UINT64_MAX);
            CHECK(topMask.has_value() && topMask->bytes == Bytes({ 0xE3, 0x08, 0x2D, 0x52, 0x77, 0x9C, 0xC1, 0xE6 }));
            CHECK(topMask.has_value() && topMask->validMask == Bytes({ 1, 1, 1, 1, 1, 1, 1, 1 }));
            // 单字节：恰好是 UINT64_MAX。
            const std::optional<Bytes> topOne = canvas.copyCachedRange(UINT64_MAX, UINT64_MAX);
            CHECK(topOne.has_value() && *topOne == Bytes({ 0xE6 }));
            // 地址空间最低端（0 页没有回填）：失败，不会因下溢回绕到最高端。
            CHECK(!canvas.copyCachedRange(0, 7).has_value());
        }

        // 返回原始缓存值、不叠加暂存补丁；补丁所在页被清出缓存后不算已加载；换代次后旧缓存作废。
        void TestRawCacheNotOverlay()
        {
            ApplyTheme(false);

            // 静态数据夹具：叠加层基线与页缓存完全一致，暂存两个字节。
            {
                auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), true, QSize(900, 420));
                HexCanvas& canvas = *fixture->canvas;
                CHECK(canvas.stageBytes(0x1010, QByteArray("\xAA\xBB", 2)));
                CHECK(canvas.cellStateAt(0x1010).value == 0xAA);
                CHECK(canvas.cellStateAt(0x1011).value == 0xBB);

                // 区间 0x100F..0x1012 = 页内偏移 15..18：(i*37+11) 低 8 位 = 36 5B 80 A5。
                // 所见值在 0x1010、0x1011 上是补丁（AA BB），缓存值仍是 5B 80。
                const std::optional<Bytes> raw = canvas.copyCachedRange(0x100F, 0x1012);
                CHECK(raw.has_value() && *raw == Bytes({ 0x36, 0x5B, 0x80, 0xA5 }));
                const std::optional<HexCanvas::CachedRangeCopy> rawMask = canvas.copyCachedRangeWithMask(0x100F, 0x1012);
                CHECK(rawMask.has_value() && rawMask->bytes == Bytes({ 0x36, 0x5B, 0x80, 0xA5 }));
                CHECK(rawMask.has_value() && rawMask->validMask == Bytes({ 1, 1, 1, 1 }));

                // 除补丁处之外，与 cellStateAt 的所见值逐字节相同。
                CHECK(raw.has_value() && raw->size() == 4);
                if (raw.has_value() && raw->size() == 4)
                {
                    CHECK((*raw)[0] == canvas.cellStateAt(0x100F).value);
                    CHECK((*raw)[1] != canvas.cellStateAt(0x1010).value);
                    CHECK((*raw)[2] != canvas.cellStateAt(0x1011).value);
                    CHECK((*raw)[3] == canvas.cellStateAt(0x1012).value);
                }
            }

            // 异步画布：暂存之后整页被清出缓存（refresh 换代次），补丁字节在屏幕上仍有值，但缓存里没有。
            {
                auto holder = MakeAsyncCanvas(0, 64ULL * 4096ULL - 1ULL, true, true);
                HexCanvas& canvas = *holder->canvas;
                CHECK(DeliverPage(canvas, 0) == HexCanvas::PageResult::Accepted);
                CHECK(canvas.stageBytes(0x10, QByteArray("\xAA", 1)));
                CHECK(canvas.cellStateAt(0x10).hasValue && canvas.cellStateAt(0x10).value == 0xAA);

                // 补丁还在、页还在：缓存值是 0x5B（偏移 16），不是补丁值。
                const std::optional<Bytes> before = canvas.copyCachedRange(0x10, 0x10);
                CHECK(before.has_value() && *before == Bytes({ 0x5B }));

                // refresh：换新代次、清空缓存。补丁还在叠加层里，所见值仍是 0xAA，但缓存已空。
                const std::uint64_t oldRevision = canvas.sourceRevision();
                canvas.refresh();
                CHECK(canvas.sourceRevision() != oldRevision);
                CHECK(canvas.cellStateAt(0x10).hasValue && canvas.cellStateAt(0x10).value == 0xAA);
                CHECK(!canvas.copyCachedRange(0x10, 0x10).has_value());
                CHECK(!canvas.copyCachedRangeWithMask(0x10, 0x10).has_value());

                // 陈旧代次的回填被拒收，缓存仍是空的；用新代次回填后才恢复。
                CHECK(canvas.deliverPage(0, MakePattern(4096), QByteArray(4096, '\x01'), oldRevision)
                    == HexCanvas::PageResult::RejectedStaleRevision);
                CHECK(!canvas.copyCachedRange(0x10, 0x10).has_value());
                CHECK(DeliverPage(canvas, 0) == HexCanvas::PageResult::Accepted);
                const std::optional<Bytes> after = canvas.copyCachedRange(0x10, 0x10);
                CHECK(after.has_value() && *after == Bytes({ 0x5B }));
            }
        }

        // 无副作用：不发 contentChanged、不换代次、不改任何字节的显示状态。
        void TestNoSideEffects()
        {
            ApplyTheme(false);
            auto holder = MakeMixedCanvas();
            HexCanvas& canvas = *holder->canvas;
            QSignalSpy spy(&canvas, &HexCanvas::contentChanged);
            const std::uint64_t revision = canvas.sourceRevision();
            const HexCanvas::CellState beforeState = canvas.cellStateAt(PageAt(3) + 20);
            const std::size_t requestsBefore = holder->provider.requests.size();

            for (int round = 0; round < 5; ++round)
            {
                canvas.copyCachedRange(PageAt(0), PageAt(2) - 1);
                canvas.copyCachedRangeWithMask(PageAt(2), PageAt(4) + 100);
                canvas.copyCachedRange(PageAt(5), PageAt(5) + 10);
            }
            Flush();
            CHECK(spy.count() == 0);
            CHECK(canvas.sourceRevision() == revision);
            CHECK(holder->provider.requests.size() == requestsBefore);
            const HexCanvas::CellState afterState = canvas.cellStateAt(PageAt(3) + 20);
            CHECK(afterState.byteState == beforeState.byteState);
            CHECK(afterState.hasValue == beforeState.hasValue);
            CHECK(afterState.value == beforeState.value);
        }

        // 与 MemoryDiffOverlay::RefreshBaseline 的数据格式衔接：掩码版的结果直接可以喂给叠加层基线。
        void TestFeedsOverlayBaseline()
        {
            ApplyTheme(false);
            auto holder = MakeMixedCanvas();
            HexCanvas& canvas = *holder->canvas;

            // 页 2 末尾 8 字节（不可读）+ 页 3 偏移 0..39（含 16 个没读到的字节），共 48 字节。
            const std::uint64_t first = PageAt(3) - 8;
            const std::optional<HexCanvas::CachedRangeCopy> copy = canvas.copyCachedRangeWithMask(first, PageAt(3) + 39);
            CHECK(copy.has_value());
            if (!copy.has_value())
            {
                return;
            }
            ksword::memwb::MemoryDiffOverlay overlay;
            const ksword::memwb::BaselineLoadStatus status =
                overlay.RefreshBaseline("memwb-copy-test", first, copy->bytes, copy->validMask);
            CHECK(status == ksword::memwb::BaselineLoadStatus::Ok);
            CHECK(overlay.BaseAddress() == first);
            CHECK(overlay.BaselineSize() == 48);

            // 没读到的位置在基线里同样是"没读到"（nullopt），而不是 0；读到的位置是缓存里的真值。
            CHECK(!overlay.BaselineByte(first).has_value());
            CHECK(!overlay.BaselineByte(first + 7).has_value());
            CHECK(overlay.BaselineByte(PageAt(3)).has_value() && *overlay.BaselineByte(PageAt(3)) == 0x0B);
            CHECK(overlay.BaselineByte(PageAt(3) + 15).has_value() && *overlay.BaselineByte(PageAt(3) + 15) == 0x36);
            CHECK(!overlay.BaselineByte(PageAt(3) + 16).has_value());
            CHECK(!overlay.BaselineByte(PageAt(3) + 31).has_value());
            CHECK(overlay.BaselineByte(PageAt(3) + 32).has_value() && *overlay.BaselineByte(PageAt(3) + 32) == 0xAB);

            // 严格版的结果同样可以直接作为全 1 掩码的基线。
            const std::optional<Bytes> strict = canvas.copyCachedRange(PageAt(0), PageAt(0) + 15);
            CHECK(strict.has_value());
            if (strict.has_value())
            {
                const ksword::memwb::BaselineLoadStatus strictStatus =
                    overlay.RefreshBaseline("memwb-copy-test", PageAt(0), *strict, Bytes(strict->size(), 1));
                CHECK(strictStatus == ksword::memwb::BaselineLoadStatus::Ok);
                CHECK(overlay.BaselineByte(PageAt(0) + 15).has_value() && *overlay.BaselineByte(PageAt(0) + 15) == 0x36);
            }
        }

        // 与 cellStateAt 逐字节对拍：同一块混合状态画布上，大量确定性随机区间的结果等于按 cellStateAt 推出的期望。
        void TestAgainstCellState()
        {
            ApplyTheme(false);
            auto holder = MakeMixedCanvas();
            HexCanvas& canvas = *holder->canvas;

            // 模型：空间前后各多取 64 字节（含空间之外），逐字节记录 cellStateAt 的结果。
            // 没有叠加层，所以 cellStateAt 的所见值就是页缓存值，两者必须逐字节一致。
            const std::uint64_t modelFirst = kBase - 64ULL;
            const std::uint64_t modelLast = PageAt(kPages) + 63ULL;
            const std::size_t modelSize = static_cast<std::size_t>(modelLast - modelFirst + 1ULL);
            std::vector<bool> inSpace(modelSize, false);
            std::vector<int> state(modelSize, 0);
            Bytes value(modelSize, 0);
            for (std::size_t index = 0; index < modelSize; ++index)
            {
                const HexCanvas::CellState cell = canvas.cellStateAt(modelFirst + index);
                inSpace[index] = cell.inSpace;
                state[index] = static_cast<int>(cell.byteState);
                value[index] = cell.value;
            }
            const int validState = static_cast<int>(HexCanvas::ByteState::Valid);
            const int unreadableState = static_cast<int>(HexCanvas::ByteState::Unreadable);

            // 确定性伪随机数（线性同余），不依赖标准库实现，换机器结果不变。
            std::uint64_t seed = 0x2545F4914F6CDD1DULL;
            const auto next = [&seed]() -> std::uint64_t {
                seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
                return seed >> 33;
            };

            int strictEngaged = 0;
            int strictRejected = 0;
            int maskEngaged = 0;
            int maskEngagedWithHoles = 0;
            int maskRejected = 0;
            int mismatches = 0;
            const int iterations = 3000;
            for (int iteration = 0; iteration < iterations; ++iteration)
            {
                // 区间选取分四种形态，保证各类结果都有足够样本。
                std::uint64_t first = 0;
                std::uint64_t last = 0;
                const std::uint64_t mode = next() % 4ULL;
                if (mode == 0)
                {
                    // 整个窗口内任意起点、长度 1..3000。
                    first = modelFirst + next() % (modelLast - modelFirst + 1ULL);
                    last = first + next() % 3000ULL;
                }
                else if (mode == 1)
                {
                    // 某一页内的任意偏移、长度 1..300（常常整段落在一个页里）。
                    first = PageAt(next() % kPages) + next() % kPage;
                    last = first + next() % 300ULL;
                }
                else if (mode == 2)
                {
                    // 跨页边界：边界前 1..64 字节到边界后 0..64 字节。
                    const std::uint64_t boundary = PageAt(1ULL + next() % (kPages - 1ULL));
                    first = boundary - (1ULL + next() % 64ULL);
                    last = boundary + next() % 65ULL;
                }
                else
                {
                    // 空间两端附近：起点在空间之前或终点在空间之后几个字节。
                    if ((next() & 1ULL) == 0ULL)
                    {
                        first = kBase - next() % 8ULL;
                        last = first + next() % 40ULL;
                    }
                    else
                    {
                        last = PageAt(kPages) - 1ULL + next() % 8ULL;
                        first = last - next() % 40ULL;
                    }
                }
                if (last > modelLast)
                {
                    last = modelLast;
                }

                // 按模型推出期望：全部在空间内且每个字节都是 Valid -> 严格版成功；
                // 全部在空间内且没有 NotLoaded/Pending（只有 Valid 与 Unreadable）-> 掩码版成功。
                bool allInSpace = true;
                bool allValid = true;
                bool allSettled = true;
                bool hasHole = false;
                Bytes expectedBytes;
                Bytes expectedMask;
                for (std::uint64_t address = first; address <= last; ++address)
                {
                    const std::size_t index = static_cast<std::size_t>(address - modelFirst);
                    if (!inSpace[index])
                    {
                        allInSpace = false;
                        break;
                    }
                    if (state[index] == validState)
                    {
                        expectedBytes.push_back(value[index]);
                        expectedMask.push_back(1);
                    }
                    else
                    {
                        allValid = false;
                        if (state[index] == unreadableState)
                        {
                            hasHole = true;
                            expectedBytes.push_back(0);
                            expectedMask.push_back(0);
                        }
                        else
                        {
                            allSettled = false;
                        }
                    }
                }
                const bool strictExpected = allInSpace && allValid;
                const bool maskExpected = allInSpace && allSettled;

                // 实际结果。
                const std::optional<Bytes> strictActual = canvas.copyCachedRange(first, last);
                const std::optional<HexCanvas::CachedRangeCopy> maskActual = canvas.copyCachedRangeWithMask(first, last);
                bool ok = (strictActual.has_value() == strictExpected) && (maskActual.has_value() == maskExpected);
                if (ok && strictExpected)
                {
                    ok = (*strictActual == expectedBytes);
                }
                if (ok && maskExpected)
                {
                    ok = (maskActual->bytes == expectedBytes) && (maskActual->validMask == expectedMask);
                }
                mismatches += ok ? 0 : 1;
                if (!ok)
                {
                    std::cerr << "cached range mismatch: [" << std::hex << first << ", " << last << std::dec << "]" << std::endl;
                }
                strictEngaged += strictExpected ? 1 : 0;
                strictRejected += strictExpected ? 0 : 1;
                maskEngaged += maskExpected ? 1 : 0;
                maskEngagedWithHoles += (maskExpected && hasHole) ? 1 : 0;
                maskRejected += maskExpected ? 0 : 1;
            }
            CHECK_NOTE(mismatches == 0, QStringLiteral("mismatches=%1").arg(mismatches));

            // 自证不是空转：各类结果都有足够多的样本。
            CHECK_NOTE(strictEngaged >= 200, QStringLiteral("strictEngaged=%1").arg(strictEngaged));
            CHECK_NOTE(strictRejected >= 200, QStringLiteral("strictRejected=%1").arg(strictRejected));
            CHECK_NOTE(maskEngaged > strictEngaged, QStringLiteral("maskEngaged=%1").arg(maskEngaged));
            CHECK_NOTE(maskEngagedWithHoles >= 100, QStringLiteral("maskEngagedWithHoles=%1").arg(maskEngagedWithHoles));
            CHECK_NOTE(maskRejected >= 200, QStringLiteral("maskRejected=%1").arg(maskRejected));
            std::cout << "cached range cross-check: " << iterations << " ranges, strict ok " << strictEngaged
                      << " / rejected " << strictRejected << ", mask ok " << maskEngaged
                      << " (with holes " << maskEngagedWithHoles << ") / rejected " << maskRejected << std::endl;
        }

        // 缓存容量整块（1 MiB）拷出：内容逐页等于 MakePattern，耗时只作量级核对；比容量多一个字节则失败。
        void TestCapacitySizedCopy()
        {
            ApplyTheme(false);
            const std::uint64_t pageCount = 256ULL;
            auto holder = MakeAsyncCanvas(0, 512ULL * 4096ULL - 1ULL, false, false);
            HexCanvas& canvas = *holder->canvas;
            bool allAccepted = true;
            for (std::uint64_t page = 0; page < pageCount; ++page)
            {
                allAccepted = allAccepted && (DeliverPage(canvas, page * 4096ULL) == HexCanvas::PageResult::Accepted);
            }
            CHECK(allAccepted);

            QElapsedTimer timer;
            timer.start();
            const std::optional<Bytes> whole = canvas.copyCachedRange(0, pageCount * 4096ULL - 1ULL);
            const qint64 strictMilliseconds = timer.elapsed();
            timer.restart();
            const std::optional<HexCanvas::CachedRangeCopy> wholeMask = canvas.copyCachedRangeWithMask(0, pageCount * 4096ULL - 1ULL);
            const qint64 maskMilliseconds = timer.elapsed();
            std::cout << "copyCachedRange 1 MiB: strict " << strictMilliseconds << " ms, with mask " << maskMilliseconds << " ms" << std::endl;

            CHECK(whole.has_value());
            CHECK(whole.has_value() && whole->size() == pageCount * 4096ULL);
            CHECK(wholeMask.has_value());
            CHECK(wholeMask.has_value() && wholeMask->bytes.size() == pageCount * 4096ULL);
            CHECK(wholeMask.has_value() && wholeMask->validMask.size() == pageCount * 4096ULL);
            if (whole.has_value() && whole->size() == pageCount * 4096ULL)
            {
                const QByteArray pattern = MakePattern(4096);
                bool allPagesEqual = true;
                for (std::uint64_t page = 0; page < pageCount; ++page)
                {
                    allPagesEqual = allPagesEqual
                        && (std::memcmp(whole->data() + page * 4096ULL, pattern.constData(), 4096) == 0);
                }
                CHECK(allPagesEqual);
            }
            if (wholeMask.has_value() && wholeMask->validMask.size() == pageCount * 4096ULL)
            {
                bool allOnes = true;
                for (const std::uint8_t flag : wholeMask->validMask)
                {
                    allOnes = allOnes && (flag == 1);
                }
                CHECK(allOnes);
            }
            // 量级核对：宽松上限，只用来抓"慢了一两个数量级"的退化。
            CHECK_NOTE(strictMilliseconds < 2000, QStringLiteral("strict ms=%1").arg(strictMilliseconds));
            CHECK_NOTE(maskMilliseconds < 2000, QStringLiteral("mask ms=%1").arg(maskMilliseconds));

            // 比容量多一个字节（落到未加载的第 256 页）、以及错开半页的同长度区间：整体失败。
            CHECK(!canvas.copyCachedRange(0, pageCount * 4096ULL).has_value());
            CHECK(!canvas.copyCachedRangeWithMask(0, pageCount * 4096ULL).has_value());
            CHECK(!canvas.copyCachedRange(2048, 2048ULL + pageCount * 4096ULL - 1ULL).has_value());
            // 去掉最后一个字节的 1 MiB - 1 字节区间成功。
            const std::optional<Bytes> almost = canvas.copyCachedRange(0, pageCount * 4096ULL - 2ULL);
            CHECK(almost.has_value() && almost->size() == pageCount * 4096ULL - 1ULL);
        }
    }

    // 入口：copyCachedRange 的全部验证，单独报告这一组的断言数与失败数。
    void RunCachedRangeTests()
    {
        const int checksBefore = g_checks;
        const int failuresBefore = g_failures;
        TestStrictValidRanges();
        TestStrictRejections();
        TestInvalidRanges();
        TestMaskVersion();
        TestPendingPage();
        TestHugeRangeAndTopOfAddressSpace();
        TestRawCacheNotOverlay();
        TestNoSideEffects();
        TestFeedsOverlayBaseline();
        TestAgainstCellState();
        TestCapacitySizedCopy();
        ApplyTheme(false);
        std::cout << "cached range tests: " << (g_checks - checksBefore) << " checks, "
                  << (g_failures - failuresBefore) << " failures" << std::endl;
    }
}
