// wpJ3_tests.SettledPages.cpp
// 作用：验证 HexCanvas::settledPageStartsInRange（Phase 3 WP-J 新增，装配接口文档
// §8 缺口 G1）。覆盖任务书点名的四种判断：页状态四种（Valid/部分有效/整页不可读/
// 未落定）、跨页边界、LRU 不被刷新、first>last 与极端区间（溢出防线）。

#include "wpJ3_common.h"

#include <QByteArray>
#include <QElapsedTimer>

namespace memwb_wpJ3_test
{
    namespace
    {
        // ---- 没有地址空间：默认构造的画布从没调用过 setAddressSpace。----
        void CheckNoAddressSpace()
        {
            HexCanvas canvas;
            const std::set<std::uint64_t> settled = canvas.settledPageStartsInRange(0, 0xFFFF);
            WPJ3_CHECK_NOTE(settled.empty(), QStringLiteral("没有地址空间时必须返回空集"));
        }

        // ---- first > last：无论地址空间是否有效都直接返回空集。----
        void CheckFirstGreaterThanLast()
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
            DeliverValidPage(canvas, 0, 0xAB);
            const std::set<std::uint64_t> settled = canvas.settledPageStartsInRange(kPageBytes, 0);
            WPJ3_CHECK_NOTE(settled.empty(), QStringLiteral("first > last 必须返回空集，即便 last 落在一个已落定页里"));

            // 跨页的 first > last（上面那条）之后再补一个"同一页内 first > last"的
            // 子情形：两端页对齐之后恰好落在同一页（firstPageStart == lastPageStart），
            // 这种情形下"页对齐后首尾颠倒"那道防线根本不会触发（页号相等不构成
            // 颠倒），必须靠最外层对原始 firstAddress/lastAddress 的比较才能挡住；
            // 否则会把"无效的反向区间"误判成"查询这一页"，错误地把 0 页的落定状态
            // 报出来。
            const std::set<std::uint64_t> samePage = canvas.settledPageStartsInRange(100, 50);
            WPJ3_CHECK_NOTE(
                samePage.empty(),
                QStringLiteral("first > last 即便两端页对齐后落在同一页，也必须返回空集"));
        }

        // ---- 独立审核 H04：first==last（查询单个地址，不是反向区间）必须正常命中
        //      该地址所在的页。这是对 first>last 早退条件本身的边界核对——如果早退
        //      写成了 first>=last（误把"相等"也当反向区间拒绝），单地址查询会被
        //      误判为空，这里直接针对"相等"这一个边界值断言。----
        void CheckSettledSingleAddressQuery()
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
            const std::uint64_t page = 0x23000000ULL; // 避开其它测试用到的地址区域
            WPJ3_CHECK(DeliverValidPage(canvas, page, 0x55) == HexCanvas::PageResult::Accepted);

            WPJ3_CHECK(canvas.settledPageStartsInRange(page + 5, page + 5).count(page) == 1);
            WPJ3_CHECK(canvas.settledPageStartsInRange(page, page).count(page) == 1);
            WPJ3_CHECK(
                canvas.settledPageStartsInRange(page + kPageBytes - 1, page + kPageBytes - 1).count(page) == 1);
            WPJ3_CHECK_NOTE(
                canvas.settledPageStartsInRange(page + kPageBytes, page + kPageBytes).empty(),
                QStringLiteral("下一页第一个地址的单点查询不该命中上一页"));
        }

        // ---- 四种页状态：Valid / 部分有效（整页仍算落定）/ 整页不可读 / 未落定。----
        void CheckFourPageStates()
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            // 地址空间给够大（1 GiB），测试区放在远离地址 0 的地方，避开 setAddressSpace
            // 自动请求可见页（落在地址 0 附近）产生的 Pending 页，避免互相干扰。
            SetupCanvas(canvas, provider, 0, 0x3FFFFFFFULL);
            const std::uint64_t base = 0x10000000ULL; // 256 MiB 偏移
            const std::uint64_t pageValid = base + 0 * kPageBytes;
            const std::uint64_t pagePartial = base + 1 * kPageBytes;
            const std::uint64_t pageUnreadable = base + 2 * kPageBytes;
            const std::uint64_t pageNotLoaded = base + 3 * kPageBytes; // 故意不碰它

            WPJ3_CHECK(DeliverValidPage(canvas, pageValid, 0x41) == HexCanvas::PageResult::Accepted);
            WPJ3_CHECK(DeliverPartialPage(canvas, pagePartial, 0x42, 16) == HexCanvas::PageResult::Accepted);
            WPJ3_CHECK(DeliverUnreadablePage(canvas, pageUnreadable) == HexCanvas::PageResult::Accepted);

            // 逐个状态核对 cellStateAt，确认夹具真的构造出了四种不同状态（不是偶然凑巧）。
            WPJ3_CHECK(canvas.cellStateAt(pageValid).byteState == HexCanvas::ByteState::Valid);
            // 部分有效页：前 16 字节 Valid，第 17 字节起 Unreadable（掩码 0）。
            WPJ3_CHECK(canvas.cellStateAt(pagePartial).byteState == HexCanvas::ByteState::Valid);
            WPJ3_CHECK(canvas.cellStateAt(pagePartial + 16).byteState == HexCanvas::ByteState::Unreadable);
            WPJ3_CHECK(canvas.cellStateAt(pageUnreadable).byteState == HexCanvas::ByteState::Unreadable);
            WPJ3_CHECK(canvas.cellStateAt(pageNotLoaded).byteState == HexCanvas::ByteState::NotLoaded);

            // 查询覆盖四页的区间：已落定的三页都在，未落定的那一页不在。
            const std::set<std::uint64_t> settled =
                canvas.settledPageStartsInRange(pageValid, pageNotLoaded + kPageBytes - 1);
            WPJ3_CHECK(settled.count(pageValid) == 1);
            WPJ3_CHECK(settled.count(pagePartial) == 1);
            WPJ3_CHECK(settled.count(pageUnreadable) == 1);
            WPJ3_CHECK_NOTE(
                settled.count(pageNotLoaded) == 0,
                QStringLiteral("NotLoaded 页不能出现在已落定集合里"));
            WPJ3_CHECK_NOTE(settled.size() == 3, QStringLiteral("四页区间应恰好命中三页"));

            // 单独查询 NotLoaded 那一页：应为空，不会被相邻的落定页"带出来"。
            const std::set<std::uint64_t> onlyNotLoaded =
                canvas.settledPageStartsInRange(pageNotLoaded, pageNotLoaded + kPageBytes - 1);
            WPJ3_CHECK(onlyNotLoaded.empty());
        }

        // ---- 跨页边界：查询区间两端都不页对齐，仍要按页对齐正确枚举到两页。----
        void CheckCrossPageBoundary()
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            SetupCanvas(canvas, provider, 0, 0x3FFFFFFFULL);
            const std::uint64_t base = 0x20000000ULL;
            const std::uint64_t pageA = base;
            const std::uint64_t pageB = base + kPageBytes;
            WPJ3_CHECK(DeliverValidPage(canvas, pageA, 0x11) == HexCanvas::PageResult::Accepted);
            WPJ3_CHECK(DeliverPartialPage(canvas, pageB, 0x22, 8) == HexCanvas::PageResult::Accepted);

            // 查询区间：pageA 末尾 96 字节 + pageB 开头 50 字节，两端都不页对齐。
            const std::set<std::uint64_t> settled =
                canvas.settledPageStartsInRange(pageA + kPageBytes - 96, pageB + 49);
            WPJ3_CHECK(settled.size() == 2);
            WPJ3_CHECK(settled.count(pageA) == 1);
            WPJ3_CHECK(settled.count(pageB) == 1);

            // 查询完全落在同一页内部（不页对齐）：只应命中这一页。
            const std::set<std::uint64_t> single =
                canvas.settledPageStartsInRange(pageA + 10, pageA + 20);
            WPJ3_CHECK(single.size() == 1);
            WPJ3_CHECK(single.count(pageA) == 1);
        }

        // ---- 命中区间中间夹一个未落定页（非连续缓存场景）：两端落定页都要出现，
        //      中间的缺口不能被"脑补"成已落定。----
        void CheckGapExcluded()
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            SetupCanvas(canvas, provider, 0, 0x3FFFFFFFULL);
            const std::uint64_t base = 0x21000000ULL;
            const std::uint64_t pageA = base;
            const std::uint64_t pageGap = base + kPageBytes;    // 故意不碰
            const std::uint64_t pageC = base + 2 * kPageBytes;
            WPJ3_CHECK(DeliverValidPage(canvas, pageA, 0x33) == HexCanvas::PageResult::Accepted);
            WPJ3_CHECK(DeliverValidPage(canvas, pageC, 0x44) == HexCanvas::PageResult::Accepted);

            const std::set<std::uint64_t> settled =
                canvas.settledPageStartsInRange(pageA, pageC + kPageBytes - 1);
            WPJ3_CHECK(settled.size() == 2);
            WPJ3_CHECK(settled.count(pageA) == 1);
            WPJ3_CHECK(settled.count(pageC) == 1);
            WPJ3_CHECK_NOTE(settled.count(pageGap) == 0, QStringLiteral("中间缺口页不能出现在结果里"));
        }

        // ---- Pending（在途）页不算落定：用 scrollToAddress 让画布自己把某页登记成
        //      在途（NoOpPageProvider 永远不回填，页会一直停在 Pending）。----
        void CheckPendingExcluded()
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            SetupCanvas(canvas, provider, 0, 0x3FFFFFFFULL);
            canvas.resize(800, 600); // 给一个确定的视口尺寸，让可见行数不是退化的 0/1。

            const std::uint64_t base = 0x22000000ULL;
            const std::uint64_t pagePending = base;
            WPJ3_CHECK(canvas.scrollToAddress(pagePending));
            WPJ3_CHECK_NOTE(
                canvas.cellStateAt(pagePending).byteState == HexCanvas::ByteState::Pending,
                QStringLiteral("scrollToAddress 之后该页应已被画布自己登记为在途"));

            const std::set<std::uint64_t> settled =
                canvas.settledPageStartsInRange(pagePending, pagePending + kPageBytes - 1);
            WPJ3_CHECK_NOTE(settled.empty(), QStringLiteral("Pending 页不是已落定页，不应出现在结果里"));
        }

        // ---- LRU 不被刷新：填满缓存容量（256 页）后查询最旧的一页，再插入第 257
        //      页触发淘汰——如果查询刷新了 LRU，被淘汰的就不会是原本最旧的那一页。----
        void CheckLruNotRefreshed()
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            // 地址空间留够 257 页以上的余量。
            SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
            const std::uint64_t base = 0x30000000ULL;
            constexpr std::size_t kCapacity = 256; // 与 HexViewport::kMaxCachedPages 一致

            std::vector<std::uint64_t> pages;
            pages.reserve(kCapacity + 1);
            for (std::size_t index = 0; index < kCapacity; ++index)
            {
                const std::uint64_t pageStart = base + static_cast<std::uint64_t>(index) * kPageBytes;
                pages.push_back(pageStart);
                WPJ3_CHECK(DeliverValidPage(canvas, pageStart, 0x55) == HexCanvas::PageResult::Accepted);
            }
            // pages[0] 是最早插入、tick 最小、理应最先被淘汰的一页。

            // 关键操作：查询恰好只覆盖 pages[0] 的区间——如果这一步偷偷刷新了 LRU，
            // pages[0] 就不再是"最久未用"。
            const std::set<std::uint64_t> probeBefore =
                canvas.settledPageStartsInRange(pages[0], pages[0] + kPageBytes - 1);
            WPJ3_CHECK(probeBefore.size() == 1 && probeBefore.count(pages[0]) == 1);

            // 再插入第 257 页：缓存已满，必须淘汰一页。
            const std::uint64_t newPage = base + static_cast<std::uint64_t>(kCapacity) * kPageBytes;
            WPJ3_CHECK(DeliverValidPage(canvas, newPage, 0x66) == HexCanvas::PageResult::Accepted);

            const bool page0StillCached =
                !canvas.settledPageStartsInRange(pages[0], pages[0] + kPageBytes - 1).empty();
            const bool page1StillCached =
                !canvas.settledPageStartsInRange(pages[1], pages[1] + kPageBytes - 1).empty();
            WPJ3_CHECK_NOTE(
                !page0StillCached,
                QStringLiteral("pages[0] 应被淘汰——如果它还在，说明某处偷偷刷新了它的 LRU"));
            WPJ3_CHECK_NOTE(
                page1StillCached,
                QStringLiteral("pages[1] 应仍然缓存着（真正被淘汰的只应是最旧的 pages[0]）"));

            // 顺带核对：覆盖全部 256 页的一次查询，结果大小恰好是 256（已经淘汰了
            // pages[0]，换进了 newPage，但这次查询区间没有包含 newPage）。
            const std::set<std::uint64_t> allOld =
                canvas.settledPageStartsInRange(pages.front(), pages.back() + kPageBytes - 1);
            WPJ3_CHECK(allOld.size() == kCapacity - 1); // 256 页区间里少了被淘汰的 pages[0]
        }

        // ---- 极端区间：first=UINT64_MAX、last=0（first>last）必须返回空集；
        //      first=0、last=UINT64_MAX 在全地址空间画布上必须瞬间返回（不逐页遍历
        //      到溢出或卡死），且没有任何落定页时结果为空。----
        void CheckExtremeRanges()
        {
            {
                HexCanvas canvas;
                NoOpPageProvider provider;
                SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
                const std::set<std::uint64_t> settled =
                    canvas.settledPageStartsInRange(0xFFFFFFFFFFFFFFFFULL, 0ULL);
                WPJ3_CHECK_NOTE(settled.empty(), QStringLiteral("first=UINT64_MAX > last=0 必须返回空集"));
            }
            {
                HexCanvas canvas;
                NoOpPageProvider provider;
                // 地址空间覆盖整个 64 位：验证页对齐算术在这个极端边界下不会回绕或卡死。
                SetupCanvas(canvas, provider, 0, 0xFFFFFFFFFFFFFFFFULL);
                QElapsedTimer timer;
                timer.start();
                const std::set<std::uint64_t> settled =
                    canvas.settledPageStartsInRange(0, 0xFFFFFFFFFFFFFFFFULL);
                const qint64 elapsedMs = timer.elapsed();
                WPJ3_CHECK_NOTE(
                    settled.empty(),
                    QStringLiteral("全地址空间画布从没交付任何页，查询全范围应为空集"));
                WPJ3_CHECK_NOTE(
                    elapsedMs < 2000,
                    QStringLiteral("查询整个 64 位地址空间必须近乎瞬间完成（不随区间大小逐页遍历）"));
            }
        }
    }

    void RunSettledPagesTests()
    {
        CheckNoAddressSpace();
        CheckFirstGreaterThanLast();
        CheckSettledSingleAddressQuery();
        CheckFourPageStates();
        CheckCrossPageBoundary();
        CheckGapExcluded();
        CheckPendingExcluded();
        CheckLruNotRefreshed();
        CheckExtremeRanges();
    }
}
