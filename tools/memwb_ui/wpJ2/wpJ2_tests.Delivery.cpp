// ============================================================
// wpJ2_tests.Delivery.cpp
// 作用：覆盖接口文档 §7 的关键判断——"部分读/零填充/不可读三种页状态的逐页
// 映射"：PageRecord::state 必须逐页核对到 deliverPage/deliverUnreadable/
// cancelPages，不得补零、不得把"读到了一部分"显示成"整页都读到了"。
// ============================================================

#include "wpJ2_common.h"

using namespace ks::ui;
using namespace ksword::memwb;

namespace wpJ2_test
{
    namespace
    {
        // Test_PartiallyValidPage_MaskReflectsPartialRead：端口只回了 2000 字节
        // 的 Partial 前缀，整页应落地成 PartiallyValid；前 2000 字节 hasValue 且
        // 等于脚本内容,其余 2096 字节不得被当成"读到了的 0"——必须仍然是
        // "没有值"（cellStateAt 的 hasValue 为假），不能静默补零显示。
        void Test_PartiallyValidPage_MaskReflectsPartialRead()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });

            const std::size_t validLength = 2000;
            const std::vector<std::uint8_t> prefix = MakePattern(0x55, validLength);
            fixture.portPtr->SetScript({MakePartial(prefix)});

            const HexFetchRange range{kFixtureFirstAddress, 1, 0};
            fixture.provider->RequestPages({range}, fixture.canvasRevision);

            const bool landed = PumpUntil(
                [&]() { return fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState != HexCanvas::ByteState::NotLoaded; },
                2000);
            WPJ2_CHECK(landed);

            // 前 2000 字节：真实读到，hasValue 且与脚本一致。
            const HexCanvas::CellState validByte = fixture.canvas->cellStateAt(kFixtureFirstAddress + validLength - 1);
            WPJ2_CHECK(validByte.byteState == HexCanvas::ByteState::Valid);
            WPJ2_CHECK(validByte.hasValue);
            WPJ2_CHECK(validByte.value == prefix[validLength - 1]);

            // 第 2000 字节（下标刚好越过前缀）：页内剩余部分应被标 Unreadable，
            // 不是补零后的"Valid 且值为 0"——这正是"不得补零"的核心判据：如果
            // 实现把整页错误地标成 Valid，这里会读到 hasValue=true 且 value=0，
            // 与"真的读到了一个 0 字节"无法区分，必须用状态本身而不是数值来判。
            const HexCanvas::CellState boundaryByte = fixture.canvas->cellStateAt(kFixtureFirstAddress + validLength);
            WPJ2_CHECK_NOTE(
                boundaryByte.byteState != HexCanvas::ByteState::Valid,
                QStringLiteral("前缀之后的字节不应被标成 Valid（可能被补零了）"));

            // 页内最后一个字节同样应该是"没有读到"，不是"读到了"。
            const HexCanvas::CellState lastByte = fixture.canvas->cellStateAt(kFixtureFirstAddress + kPageSize - 1);
            WPJ2_CHECK(lastByte.byteState != HexCanvas::ByteState::Valid);
        }

        // Test_UnreadablePage_DeliversUnreadableNotFakeZero：端口报 Unreadable，
        // 整页应落地成"不可读"状态，不是"读到了全 0"——两者在 UI 上都可能画成
        // 看起来空白，但状态必须不同（否则用户没法区分"这块内存全是 0"与
        // "这块内存读不到"）。
        void Test_UnreadablePage_DeliversUnreadableNotFakeZero()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });
            fixture.portPtr->SetScript({MakeUnreadable("page protection denies access")});

            const HexFetchRange range{kFixtureFirstAddress, 1, 0};
            fixture.provider->RequestPages({range}, fixture.canvasRevision);

            const bool landed = PumpUntil(
                [&]() { return fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState != HexCanvas::ByteState::NotLoaded; },
                2000);
            WPJ2_CHECK(landed);

            const HexCanvas::CellState cell = fixture.canvas->cellStateAt(kFixtureFirstAddress);
            WPJ2_CHECK(cell.byteState == HexCanvas::ByteState::Unreadable);
            WPJ2_CHECK(!cell.hasValue);
        }

        // Test_MultiPageRange_EachPageMappedIndependently：一次请求两页（跨
        // 一个 FetchRange，pageCount=2），第一页 Ok、第二页 Unreadable——必须
        // 逐页分别落地，不能把两页混成同一个状态。
        void Test_MultiPageRange_EachPageMappedIndependently()
        {
            ProviderFixture fixture = MakeProviderFixture(Channel::StandardDriver);
            fixture.provider->setGateInputsProvider(
                [channel = Channel::StandardDriver]() { return MakeAvailableGateInputs(channel); });

            // maxReadBytes 设为正好一页，逼着 MemoryPageReader 把两页请求拆成
            // 两次独立的 Read 调用（否则默认不限长度会把两页合并成一次 8192
            // 字节的请求，脚本就只需要一条，但那样测不出"按页拆分"这件事）。
            fixture.portPtr->SetLimits(IoLimits{kPageSize, 0});
            const std::vector<std::uint8_t> firstPagePattern = MakePattern(0x20, static_cast<std::size_t>(kPageSize));
            fixture.portPtr->SetScript({MakeOk(firstPagePattern), MakeUnreadable()});

            const HexFetchRange range{kFixtureFirstAddress, 2, 0};
            fixture.provider->RequestPages({range}, fixture.canvasRevision);

            const bool landed = PumpUntil(
                [&]()
                {
                    return fixture.canvas->cellStateAt(kFixtureFirstAddress).byteState != HexCanvas::ByteState::NotLoaded
                        && fixture.canvas->cellStateAt(kFixtureFirstAddress + kPageSize).byteState
                               != HexCanvas::ByteState::NotLoaded;
                },
                2000);
            WPJ2_CHECK(landed);
            WPJ2_CHECK(fixture.portPtr->CallCount() == 2);

            const HexCanvas::CellState pageOneByte = fixture.canvas->cellStateAt(kFixtureFirstAddress);
            WPJ2_CHECK(pageOneByte.byteState == HexCanvas::ByteState::Valid);
            WPJ2_CHECK(pageOneByte.value == firstPagePattern[0]);

            const HexCanvas::CellState pageTwoByte = fixture.canvas->cellStateAt(kFixtureFirstAddress + kPageSize);
            WPJ2_CHECK_NOTE(
                pageTwoByte.byteState == HexCanvas::ByteState::Unreadable,
                QStringLiteral("第二页应独立标为 Unreadable，不受第一页 Valid 影响"));
        }
    }

    void RunDeliveryTests()
    {
        Test_PartiallyValidPage_MaskReflectsPartialRead();
        Test_UnreadablePage_DeliversUnreadableNotFakeZero();
        Test_MultiPageRange_EachPageMappedIndependently();
    }
}
