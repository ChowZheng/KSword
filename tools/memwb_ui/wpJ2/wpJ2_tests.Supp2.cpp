// ============================================================
// wpJ2_tests.Supp2.cpp —— 第二轮独立审核（review2-wpJ2.md）补测，经本包第二轮
// 修复后并入默认运行。两组：
//  - G 组（默认运行，必须通过）：G1-G17 是审核者原样验证过的补测；G18-G23 是
//    本包把审核者原来的"缺陷复现"（R1/R2/R3/R5/R6）按修复后的真实行为改写、
//    转正为"必须通过"（G18-G22 对应 N1/N2/N3/N5/N6；G23 替代 N4——其原始断言
//    "resetScratchLatch+滚动即可自动恢复"已被裁决否决，改断言"必须显式
//    retryAllFailed 才能恢复"）；G24 补"同一个 channelFailed range 内部分页
//    已读成功"这个 N3 子场景。
//  - R 组（WPJ2R2_REPRO=1 才运行）：仍未修复、按裁决延后处理的缺陷（N7/N8），
//    以及一条未定论的可疑点，详见 fix2-wpJ2.md §7。
// 接入：build-wpJ2-tests.cmd 加 "%FIX%\wpJ2_tests.Supp2.cpp"；wpJ2_common.h
//       声明 RunSupp2Tests()；wpJ2_main.cpp 调用它。
// ============================================================

#include "wpJ2_common.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QScrollBar>

#include <chrono>
#include <stdexcept>
#include <thread>

using namespace ks::ui;
using namespace ksword::memwb;

namespace wpJ2_test
{
    namespace
    {
        using State = HexCanvas::ByteState;

        std::vector<std::uint8_t> OnePage(std::uint8_t seed)
        {
            return MakePattern(seed, static_cast<std::size_t>(kPageSize));
        }

        std::vector<IoReadResult> ManyOk(int count)
        {
            std::vector<IoReadResult> script;
            for (int index = 0; index < count; ++index)
            {
                script.push_back(MakeOk(OnePage(static_cast<std::uint8_t>(index))));
            }
            return script;
        }

        void GateOk(ProviderFixture& fx, Channel channel)
        {
            fx.provider->setGateInputsProvider([channel]() { return MakeAvailableGateInputs(channel); });
        }

        void ShowCanvas(ProviderFixture& fx)
        {
            fx.canvas->resize(900, 300);
            fx.canvas->show();
            QCoreApplication::processEvents();
        }

        void FreezePaint(ProviderFixture& fx)
        {
            fx.canvas->viewport()->setUpdatesEnabled(false);
        }

        void Pump(int ms)
        {
            PumpUntil([] { return false; }, ms);
        }

        void BusyWaitCalls(FakeAsyncMemoryIoPort* port, int count, int ms)
        {
            QElapsedTimer timer;
            timer.start();
            while (port->CallCount() < count && timer.elapsed() < ms)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // 让两条数轴数值不同（画布轴多换两次空间，目标轴不动）。默认夹具里两条轴恰好都是 2，
        // 任何"误用另一条轴"的变异在默认夹具里都不可观察——这是第二轮发现的最大夹具缺口。
        // 必须在 GateOk 之前调用：setAddressSpace 会立即发起一次请求。
        void SkewAxes(ProviderFixture& fx)
        {
            fx.canvas->setAddressSpace(kFixtureFirstAddress, kFixtureLastAddress);
            fx.canvas->setAddressSpace(kFixtureFirstAddress, kFixtureLastAddress);
            fx.canvasRevision = fx.canvas->sourceRevision();
        }

        bool ReproEnabled()
        {
            return qEnvironmentVariableIntValue("WPJ2R2_REPRO") != 0;
        }

        // ------------------------------------------------------------------
        // 会抛异常的端口：mode 0=Read 抛 std::runtime_error；1=Read 抛 int；2=Limits 抛 std::runtime_error。
        // ------------------------------------------------------------------
        class ThrowingPort final : public IMemoryIoPort
        {
        public:
            explicit ThrowingPort(int mode)
                : mode_(mode)
            {
            }

            IoLimits Limits(const MemoryTargetSession&) const override
            {
                if (mode_ == 2)
                {
                    throw std::runtime_error("Limits threw");
                }
                return IoLimits{};
            }

            IoReadResult Read(const MemoryTargetSession&, std::uint64_t, std::uint64_t) override
            {
                if (mode_ == 1)
                {
                    throw 42;
                }
                throw std::runtime_error("Read threw");
            }

            IoWriteResult Write(const MemoryTargetSession&, std::uint64_t, const std::vector<std::uint8_t>&, bool) override
            {
                return IoWriteResult{};
            }

        private:
            int mode_;
        };

        // ================================================================
        // G 组：必须通过
        // ================================================================

        // G1（杀 rA09 去掉 try/catch、rC14 catch(...) 不标 channelFailed）：端口抛异常时进程不得终止，
        // 且要转成 readFailed。夹具原来没有任何抛异常的端口。
        void Test_S2_G1_PortExceptionBecomesReadFailed()
        {
            const char* expected[3] = {
                "worker exception: Read threw", "worker exception: unknown", "worker exception: Limits threw"};
            for (int mode = 0; mode < 3; ++mode)
            {
                KernelTargetFixture tf = MakeKernelTarget(Channel::StandardDriver);
                WorkbenchPageProvider::IoPortFactory factory = [mode]() -> std::unique_ptr<IMemoryIoPort>
                { return std::make_unique<ThrowingPort>(mode); };
                auto provider = std::make_unique<WorkbenchPageProvider>(std::move(factory), tf.target.get());
                auto canvas = std::make_unique<HexCanvas>();
                provider->setCanvas(canvas.get());
                canvas->setPageProvider(provider.get());
                canvas->setAddressSpace(kFixtureFirstAddress, kFixtureLastAddress);
                provider->setGateInputsProvider([] { return MakeAvailableGateInputs(Channel::StandardDriver); });
                int failed = 0;
                QString text;
                QObject::connect(provider.get(), &WorkbenchPageProvider::readFailed, [&](quint64, QString t) {
                    ++failed;
                    text = t;
                });
                provider->RequestPages({HexFetchRange{0, 1, 0}, HexFetchRange{kPageSize, 1, 0}}, canvas->sourceRevision());
                WPJ2_CHECK_NOTE(PumpUntil([&] { return failed > 0; }, 2000), QStringLiteral("mode %1：端口抛异常应转成 readFailed").arg(mode));
                WPJ2_CHECK_NOTE(
                    text.contains(QString::fromLatin1(expected[mode])),
                    QStringLiteral("mode %1：失败文本应含 '%2'，实际 '%3'").arg(mode).arg(QString::fromLatin1(expected[mode])).arg(text));
                WPJ2_CHECK(failed == 1);
            }
        }

        // G2（杀 rC08）readFailed 带的必须是画布轴代次——两条轴数值不同时才可观察。
        void Test_S2_G2_ReadFailedCarriesCanvasAxis()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            SkewAxes(fx);
            GateOk(fx, Channel::StandardDriver);
            const std::uint64_t targetRevision = fx.targetFixture.target->capture().rev.source;
            WPJ2_CHECK_NOTE(fx.canvasRevision != targetRevision, QStringLiteral("前提：两条轴此刻应该不相等"));
            quint64 got = 0;
            bool failed = false;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::readFailed, [&](quint64 revision, QString) {
                got = revision;
                failed = true;
            });
            fx.portPtr->SetScript({MakeFailed("x")});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return failed; }, 2000));
            WPJ2_CHECK_NOTE(got == fx.canvasRevision, QStringLiteral("readFailed 应携带画布轴 %1，实际 %2").arg(fx.canvasRevision).arg(got));
        }

        // G3（杀 rC07、rC10）闩锁按目标轴键、retryBlockedByLatch 带画布轴——两条轴数值不同时才可观察。
        void Test_S2_G3_LatchKeyIsTargetAxis_SignalCarriesCanvasAxis()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            SkewAxes(fx);
            GateOk(fx, Channel::Ddma);
            WPJ2_CHECK(fx.canvasRevision != fx.targetFixture.target->capture().rev.source);
            int latched = 0;
            int blocked = 0;
            quint64 blockedRevision = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::scratchAreaDirtyLatched, [&] { ++latched; });
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::retryBlockedByLatch, [&](quint64 revision) {
                ++blocked;
                blockedRevision = revision;
            });
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(1)), MakeOk(OnePage(2))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return latched == 1; }, 2000));
            const int before = fx.portPtr->CallCount();
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            Pump(150);
            WPJ2_CHECK_NOTE(fx.portPtr->CallCount() == before, QStringLiteral("同一目标代次内闩锁必须拦住第二次请求（两轴不同）"));
            WPJ2_CHECK(blocked >= 1);
            WPJ2_CHECK_NOTE(blockedRevision == fx.canvasRevision, QStringLiteral("retryBlockedByLatch 应携带画布轴"));
        }

        // G4（杀 rC39）目标代次前进、闩锁释放之后，新代次内再次报脏必须重新置闩锁并再发一次信号。
        void Test_S2_G4_LatchReLatchesOnNewTargetRevision()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            int latched = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::scratchAreaDirtyLatched, [&] { ++latched; });
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(1))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return latched == 1; }, 2000));
            fx.targetFixture.target->requestReload(); // 目标轴前进：闩锁对新代次失效
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(2))});
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK_NOTE(PumpUntil([&] { return latched == 2; }, 2000), QStringLiteral("新目标代次内再次报脏应重新置闩锁并发信号，实际 %1 次").arg(latched));
        }

        // G5（杀 rC09）cancelAllInFlight 必须按画布轴释放"排队中任务"登记的 Pending——两轴不同时才可观察。
        void Test_S2_G5_CancelAllReleasesPendingWhenAxesDiffer()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            SkewAxes(fx);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetDelayMs(150);
            fx.portPtr->SetScript(ManyOk(8));
            ShowCanvas(fx);
            FreezePaint(fx);
            WPJ2_CHECK_NOTE(fx.canvas->cellStateAt(0).byteState == State::Pending, QStringLiteral("前提：画布已登记在途"));
            fx.provider->cancelAllInFlight();
            WPJ2_CHECK_NOTE(
                fx.canvas->cellStateAt(0).byteState == State::NotLoaded,
                QStringLiteral("cancelAllInFlight 应同步释放在途登记，实际 %1").arg(static_cast<int>(fx.canvas->cellStateAt(0).byteState)));
        }

        // G6（杀 rC36）cancelAllInFlight 必须真的让池停手：排队中的任务不得再碰端口。
        void Test_S2_G6_CancelAllStopsQueuedPortReads()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetDelayMs(100);
            fx.portPtr->SetScript(ManyOk(8));
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            BusyWaitCalls(fx.portPtr, 1, 2000);
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision); // 排队中
            fx.provider->cancelAllInFlight();
            Pump(600);
            WPJ2_CHECK_NOTE(
                fx.portPtr->CallCount() == 1,
                QStringLiteral("取消之后排队任务不得再读端口，实际端口调用 %1").arg(fx.portPtr->CallCount()));
        }

        // G7（杀 rC11）被 cancelAllInFlight 丢弃的排队任务不得在 pendingJobs_ 里留下"永久在途"的记账。
        void Test_S2_G7_RereadAfterCancelAllIsNotSwallowed()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetDelayMs(200);
            fx.portPtr->SetScript(ManyOk(8));
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            BusyWaitCalls(fx.portPtr, 1, 2000);
            fx.provider->RequestPages({HexFetchRange{3 * kPageSize, 1, 0}}, fx.canvasRevision); // 排队，从未跑过
            fx.provider->cancelAllInFlight();
            Pump(500);
            const int before = fx.portPtr->CallCount();
            fx.provider->rereadByteRange(3 * kPageSize, 8);
            WPJ2_CHECK_NOTE(
                PumpUntil([&] { return fx.portPtr->CallCount() > before; }, 1500),
                QStringLiteral("被丢弃的排队任务的记账不得吞掉之后对同一页的重读"));
        }

        // G8（杀 rC03）字节范围恰好以页边界结束时只能读一页。
        void Test_S2_G8_RereadExactlyOnePage()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetScript(ManyOk(8));
            fx.provider->rereadByteRange(kPageSize, kPageSize);
            Pump(300);
            WPJ2_CHECK_NOTE(fx.portPtr->CallCount() == 1, QStringLiteral("(4096,4096) 恰好一页，实际端口调用 %1").arg(fx.portPtr->CallCount()));
            fx.provider->rereadByteRange(3 * kPageSize - 1, 2); // 跨页 2/3
            Pump(300);
            WPJ2_CHECK_NOTE(fx.portPtr->CallCount() == 3, QStringLiteral("跨两页的 2 字节应再读 2 页，实际累计 %1").arg(fx.portPtr->CallCount()));
        }

        // G9（杀 rC04）length==0 必须是空操作（下溢会变成"整个地址空间"）。
        void Test_S2_G9_RereadZeroLengthIsNoOp()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetScript(ManyOk(8));
            fx.provider->rereadByteRange(0, 0);
            fx.provider->rereadByteRange(12345, 0);
            Pump(300);
            WPJ2_CHECK_NOTE(fx.portPtr->CallCount() == 0, QStringLiteral("length==0 不得发起读取，实际端口调用 %1").arg(fx.portPtr->CallCount()));
        }

        // G10（杀 rC02）去重的上界不得多算一页：紧邻在途范围之后的页必须读。
        void Test_S2_G10_DedupDoesNotSwallowAdjacentPage()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetDelayMs(150);
            fx.portPtr->SetScript(ManyOk(8));
            fx.provider->rereadByteRange(0, kPageSize);  // 页 0，在途
            fx.provider->rereadByteRange(kPageSize, 1);  // 页 1，紧邻
            Pump(700);
            WPJ2_CHECK_NOTE(fx.portPtr->CallCount() == 2, QStringLiteral("相邻页不得被去重吞掉，实际端口调用 %1").arg(fx.portPtr->CallCount()));
        }

        // G11（杀 rC06）去抖只吞"同一原因"：原因变化必须重发。
        void Test_S2_G11_DebounceEmitsAgainWhenReasonChanges()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            fx.portPtr->SetScript({MakeOk(OnePage(1))});
            fx.provider->RequestPages({HexFetchRange{8 * kPageSize, 1, 0}}, fx.canvasRevision); // 可用：清去抖记忆
            Pump(100);
            std::vector<GateReason> reasons;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::channelUnavailable, [&](GateVerdict verdict) { reasons.push_back(verdict.reason); });
            fx.provider->setGateInputsProvider([] { return GateInputs{}; }); // 驱动未加载
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision); // 同原因：被去抖
            fx.provider->setGateInputsProvider(
                []
                {
                    GateInputs inputs;
                    inputs.driverLoaded = true;
                    inputs.ddmaSessionReady = false;
                    return inputs;
                }); // 会话未就绪：原因变了
            fx.provider->RequestPages({HexFetchRange{2 * kPageSize, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(reasons.size() == 2);
            if (reasons.size() == 2)
            {
                WPJ2_CHECK(reasons[0] == GateReason::DriverNotLoaded);
                WPJ2_CHECK(reasons[1] == GateReason::SessionNotReady);
            }
        }

        // G12（杀 rC27）target 为空时 RequestPages 必须撤销画布刚登记的在途，而不是留下 Pending。
        void Test_S2_G12_NullTargetReleasesRegistrations()
        {
            auto* rawPort = new FakeAsyncMemoryIoPort();
            WorkbenchPageProvider::IoPortFactory factory = [rawPort]() -> std::unique_ptr<IMemoryIoPort>
            { return std::unique_ptr<IMemoryIoPort>(rawPort); };
            auto provider = std::make_unique<WorkbenchPageProvider>(std::move(factory), nullptr);
            auto canvas = std::make_unique<HexCanvas>();
            provider->setCanvas(canvas.get());
            canvas->setPageProvider(provider.get());
            canvas->viewport()->setUpdatesEnabled(false);
            canvas->setAddressSpace(kFixtureFirstAddress, kFixtureLastAddress); // 立即请求可见页 -> provider 无 target
            WPJ2_CHECK_NOTE(
                canvas->cellStateAt(0).byteState == State::NotLoaded,
                QStringLiteral("无 target 时在途登记应被撤销，实际 %1").arg(static_cast<int>(canvas->cellStateAt(0).byteState)));
            WPJ2_CHECK(rawPort->CallCount() == 0);
        }

        // G13（杀 rC29/rC30）画布为空（暂停回填）时 retryFailedRanges / rereadByteRange 必须是空操作而不是解引用空指针。
        void Test_S2_G13_NullCanvasRetryAndRereadAreNoOps()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetScript(ManyOk(4));
            fx.provider->setCanvas(nullptr);
            fx.provider->retryFailedRanges({HexFetchRange{0, 1, 0}});
            fx.provider->rereadByteRange(0, 8);
            Pump(200);
            WPJ2_CHECK(fx.portPtr->CallCount() == 0);
        }

        // G14（杀 rC23/rC24）cancelAllInFlight 之后新提交的请求必须能落地（任务代次快照与落地比较必须同源）。
        // 夹具里没有任何用例"先 cancelAllInFlight、再请求、再看交付"，代次相关的两处变异全部幸存。
        void Test_S2_G14_RequestAfterCancelAllIsDelivered()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetScript({MakeOk(OnePage(0x21)), MakeOk(OnePage(0x22))});
            fx.provider->cancelAllInFlight();
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK_NOTE(
                PumpUntil([&] { return fx.canvas->cellStateAt(0).byteState == State::Valid; }, 2000),
                QStringLiteral("cancelAllInFlight 之后新提交的请求应正常落地"));
            fx.provider->cancelAllInFlight();
            fx.provider->cancelAllInFlight();
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK_NOTE(
                PumpUntil([&] { return fx.canvas->cellStateAt(kPageSize).byteState == State::Valid; }, 2000),
                QStringLiteral("多次 cancelAllInFlight 之后新提交的请求应正常落地"));
        }

        // G15（杀 rC16）闩锁前置判定拦下请求时必须撤销画布刚登记的在途，不能把页留在 Pending（之后永不重试）。
        void Test_S2_G15_LatchBlockReleasesRegistrations()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            int latched = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::scratchAreaDirtyLatched, [&] { ++latched; });
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(1))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return latched == 1; }, 2000));
            ShowCanvas(fx); // 画布登记预取页在途 -> provider 被闩锁拦住
            FreezePaint(fx);
            WPJ2_CHECK_NOTE(
                fx.canvas->cellStateAt(kPageSize).byteState == State::NotLoaded,
                QStringLiteral("闩锁拦截后在途登记应被撤销，实际 %1").arg(static_cast<int>(fx.canvas->cellStateAt(kPageSize).byteState)));
        }

        // G16/G17（杀 rW1/rW2，wpJ5 在同一文件追加的 jobLanded / hasInFlightRequests；wpJ2 夹具原先完全没有覆盖）
        void Test_S2_G16_JobLandedAndHasInFlightRequests()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            int landed = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::jobLanded, [&] { ++landed; });
            WPJ2_CHECK(!fx.provider->hasInFlightRequests());
            fx.portPtr->SetDelayMs(80);
            fx.portPtr->SetScript({MakeOk(OnePage(1)), MakeFailed("x")});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK_NOTE(fx.provider->hasInFlightRequests(), QStringLiteral("一个任务在途时 hasInFlightRequests 应为真"));
            WPJ2_CHECK(PumpUntil([&] { return landed == 1; }, 2000));
            WPJ2_CHECK(!fx.provider->hasInFlightRequests());
            // 失败分支也要恰好发一次 jobLanded
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK_NOTE(PumpUntil([&] { return landed == 2; }, 2000), QStringLiteral("channelFailed 分支漏发 jobLanded，实际 %1 次").arg(landed));
            Pump(100);
            WPJ2_CHECK(landed == 2);
            WPJ2_CHECK(!fx.provider->hasInFlightRequests());
        }

        void Test_S2_G17_HasInFlightFalseRightAfterCancelAll()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetDelayMs(120);
            fx.portPtr->SetScript(ManyOk(4));
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(fx.provider->hasInFlightRequests());
            fx.provider->cancelAllInFlight();
            WPJ2_CHECK_NOTE(!fx.provider->hasInFlightRequests(), QStringLiteral("cancelAllInFlight 之后不应再有在途记账"));
        }

        // G18（原 R1，N1 已修好）：sessionChanged 的槛里摘掉画布，落地路径不得再解引用空 QPointer。
        void Test_S2_G18_ReentrantSlotDetachesCanvas_NoCrash()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            fx.targetFixture.services->SetDdmaGeneration(1);
            (void)fx.targetFixture.target->session();
            fx.portPtr->SetDelayMs(60);
            fx.portPtr->SetScript({MakeOk(OnePage(0x66))});
            QObject::connect(fx.targetFixture.target.get(), &WorkbenchTarget::sessionChanged, [&](quint32) { fx.provider->setCanvas(nullptr); });
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            fx.targetFixture.services->SetDdmaGeneration(2);
            Pump(500);
            WPJ2_CHECK(true); // 能走到这里即未崩溃（N1：isSourceFresh 之后重新判空 canvas_）
        }

        // G19（原 R2，N2 已修好）：在途读取已经真正开始（已消费脚本第一条）之后的
        // rereadByteRange 不得被去重吞掉——写后重读必须真的发起一次新读取。
        void Test_S2_G19_DedupMustNotSwallowRereadWhenInFlightReadAlreadyStarted()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetDelayMs(200);
            fx.portPtr->SetScript({MakeOk(OnePage(0xAA)), MakeOk(OnePage(0xBB))}); // 写前 / 写后
            fx.provider->rereadByteRange(0, 1);
            BusyWaitCalls(fx.portPtr, 1, 2000); // 等第一个任务真正开始读（started_ 已置位）
            fx.provider->rereadByteRange(0, 1); // 写提交 -> 重读：不应被去重吞掉
            PumpUntil([&] { return fx.canvas->cellStateAt(0).byteState == State::Valid; }, 2000);
            Pump(500);
            WPJ2_CHECK_NOTE(
                fx.canvas->cellStateAt(0).value == 0xBB,
                QStringLiteral("写之后的重读被去重吞掉：画面停在 0x%1").arg(static_cast<int>(fx.canvas->cellStateAt(0).value), 2, 16, QLatin1Char('0')));
        }

        // G20（原 R3，N3 已修好）：同批里已经读成功的可见页不得被另一个失败的预取
        // range 连带丢弃。
        void Test_S2_G20_SuccessfulVisibleRangeSurvivesPrefetchFailure()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetScript({MakeOk(OnePage(0x31)), MakeFailed("prefetch boom")});
            bool failed = false;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::readFailed, [&] { failed = true; });
            ShowCanvas(fx);
            PumpUntil([&] { return failed; }, 2000);
            Pump(200);
            WPJ2_CHECK_NOTE(
                fx.canvas->cellStateAt(0).byteState == State::Valid,
                QStringLiteral("读成功的可见页被连带丢弃，实际 %1").arg(static_cast<int>(fx.canvas->cellStateAt(0).byteState)));
        }

        // G21（原 R5，N5 已修好）：闩锁路径与 Gate 路径一样要去抖——同一（闩锁目标轴，
        // 画布轴）组合连续命中，普通重绘不得反复发 retryBlockedByLatch。
        void Test_S2_G21_LatchPathSignalIsDebouncedLikeGatePath()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            int latched = 0;
            int blocked = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::scratchAreaDirtyLatched, [&] { ++latched; });
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::retryBlockedByLatch, [&] { ++blocked; });
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(0x41))});
            ShowCanvas(fx);
            PumpUntil([&] { return latched > 0; }, 2000);
            Pump(200);
            auto* bar = fx.canvas->verticalScrollBar();
            bar->setValue(bar->maximum() / 2);
            Pump(200);
            const int before = blocked;
            for (int i = 0; i < 5; ++i)
            {
                fx.canvas->viewport()->repaint();
                Pump(40);
            }
            WPJ2_CHECK_NOTE(blocked - before <= 1, QStringLiteral("5 次普通重绘发了 %1 次 retryBlockedByLatch").arg(blocked - before));
        }

        // G22（原 R6，N6 已修好）：setCanvas(nullptr) 暂停期间落地的结果被记进
        // pausedPendingRanges_，恢复（setCanvas 非空）之后补 cancelPages，重绘能
        // 重新把它们排进请求，不再永远卡在 Pending。
        void Test_S2_G22_PauseResumeDoesNotLeavePending()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetDelayMs(120);
            fx.portPtr->SetScript(ManyOk(8));
            ShowCanvas(fx);
            BusyWaitCalls(fx.portPtr, 1, 2000);
            fx.provider->setCanvas(nullptr);
            Pump(500);
            fx.provider->setCanvas(fx.canvas.get());
            // repaint 一次即可触发 paintEvent 的 0ms 定时器补读（见
            // HexCanvas.Paint.cpp 的 m_refetchQueued），真正落地要等端口的
            // 120 ms 延迟走完，用 PumpUntil 等，不按固定次数/时长猜。
            fx.canvas->viewport()->repaint();
            WPJ2_CHECK_NOTE(
                PumpUntil([&] { return fx.canvas->cellStateAt(0).byteState != State::Pending; }, 3000),
                QStringLiteral("暂停期间被丢弃的结果让页永远 Pending"));
        }

        // G23（替代原 R4；N4 的裁决是"不自动恢复，必须显式 retryAllFailed"）：
        // 被毒标志跳过的 range 的页在 resetScratchLatch + 滚走滚回之后仍必须保持
        // Pending（这是既定行为，不是缺陷）；只有调用 retryAllFailed() 才会真的
        // 再问一次端口并恢复。
        void Test_S2_G23_SkippedRangeRequiresExplicitRetryAllFailed()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            int latched = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::scratchAreaDirtyLatched, [&] { ++latched; });
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(0x41)), MakeOk(OnePage(0x42)), MakeOk(OnePage(0x43))});
            ShowCanvas(fx);
            PumpUntil([&] { return latched > 0; }, 2000);
            Pump(200);
            fx.provider->resetScratchLatch();
            auto* bar = fx.canvas->verticalScrollBar();
            bar->setValue(bar->maximum() / 8);
            Pump(200);
            bar->setValue(0);
            Pump(300);
            WPJ2_CHECK_NOTE(
                fx.canvas->cellStateAt(kPageSize).byteState == State::Pending,
                QStringLiteral("裁决：滚走滚回不应自动恢复，实际 %1").arg(static_cast<int>(fx.canvas->cellStateAt(kPageSize).byteState)));
            WPJ2_CHECK_NOTE(!fx.provider->failedRanges().empty(), QStringLiteral("被跳过的 range 应该记进 failedRanges_"));
            fx.provider->retryAllFailed();
            WPJ2_CHECK_NOTE(
                PumpUntil([&] { return fx.canvas->cellStateAt(kPageSize).byteState != State::Pending; }, 2000),
                QStringLiteral("retryAllFailed 之后应该真的再问一次端口并恢复"));
        }

        // ================================================================
        // R 组：仍未修复/未定论，留作已知风险（WPJ2R2_REPRO=1 才运行）
        // ================================================================

        // N8（低，窄窗口，已记录，本次未修）：UI 线程忙、脏扇区的完成事件还没
        // 落地时，新请求会再读一次脏暂存区——ResetPoison() 按"每次提交"重置，
        // 修法需要改成按 epoch 重置，风险是会影响已经钉住的 D3 行为，详见
        // fix2-wpJ2.md §7。
        void Test_S2_R7_NewRequestBeforeDirtyLandingMustNotReadScratchAgain()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            fx.portPtr->SetDelayMs(40);
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(1)), MakeOk(OnePage(2))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            BusyWaitCalls(fx.portPtr, 1, 2000);
            std::this_thread::sleep_for(std::chrono::milliseconds(120)); // 读完，完成事件还没处理
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            Pump(300);
            WPJ2_CHECK_NOTE(fx.portPtr->CallCount() == 1, QStringLiteral("脏暂存区被再读，实际端口调用 %1").arg(fx.portPtr->CallCount()));
        }

        // 可疑点 1（未定论，不计入 N1-N8，本次未修）：去抖记忆不随身份变化清除——
        // 换通道后若新通道恰好报同一个 GateReason，不会重新提示。
        void Test_S2_R8_DebounceResetsOnIdentityChange()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetScript({MakeOk(OnePage(1))});
            fx.provider->RequestPages({HexFetchRange{8 * kPageSize, 1, 0}}, fx.canvasRevision);
            Pump(100);
            fx.provider->setGateInputsProvider(nullptr);
            int count = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::channelUnavailable, [&] { ++count; });
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            fx.targetFixture.target->requestChannel(Channel::Ddma);
            fx.provider->cancelAllInFlight();
            fx.canvas->setAddressSpace(kFixtureFirstAddress, kFixtureLastAddress);
            WPJ2_CHECK_NOTE(count == 2, QStringLiteral("换通道后同原因的不可用应重新提示，累计 %1 次").arg(count));
        }

        // N7（中，契约冲突，已记录，本次未修，超出本包范围）：
        // WorkbenchWriteController 把所有真正落地的块聚合成 [最小起点, 最大终点)
        // 一次性交给 rereadByteRange，而本包只能按"从起点数的前 256 页"夹取；
        // 真正的修法要求改动 WorkbenchWriteController（不属于本包文件）或者
        // 让 rereadByteRange 接受多段范围，详见 fix2-wpJ2.md §7。
        void Test_S2_R9_RereadSpanBeyondCacheStillCoversLastBlock()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetScript(ManyOk(400));
            const std::uint64_t blockAPage = 2;
            const std::uint64_t blockBPage = 300; // 与块 A 相距 298 页 > 256 页缓存容量
            // 与 WorkbenchWriteController::HandleCommitReport 同一种聚合：[A 起点, B 终点)
            const std::uint64_t rangeStart = blockAPage * kPageSize + 16;
            const std::uint64_t rangeEnd = blockBPage * kPageSize + 16 + 4;
            fx.provider->rereadByteRange(rangeStart, rangeEnd - rangeStart);
            Pump(1500);
            bool coveredB = false;
            for (const ReadCall& call : fx.portPtr->Calls())
            {
                if (call.address <= blockBPage * kPageSize + 16 && blockBPage * kPageSize + 16 < call.address + call.length)
                {
                    coveredB = true;
                }
            }
            WPJ2_CHECK_NOTE(coveredB, QStringLiteral("块 B（页 300）所在页没有被重读，端口调用 %1 次").arg(fx.portPtr->CallCount()));
        }
    }

    void RunSupp2Tests()
    {
        Test_S2_G1_PortExceptionBecomesReadFailed();
        Test_S2_G2_ReadFailedCarriesCanvasAxis();
        Test_S2_G3_LatchKeyIsTargetAxis_SignalCarriesCanvasAxis();
        Test_S2_G4_LatchReLatchesOnNewTargetRevision();
        Test_S2_G5_CancelAllReleasesPendingWhenAxesDiffer();
        Test_S2_G6_CancelAllStopsQueuedPortReads();
        Test_S2_G7_RereadAfterCancelAllIsNotSwallowed();
        Test_S2_G8_RereadExactlyOnePage();
        Test_S2_G9_RereadZeroLengthIsNoOp();
        Test_S2_G10_DedupDoesNotSwallowAdjacentPage();
        Test_S2_G11_DebounceEmitsAgainWhenReasonChanges();
        Test_S2_G12_NullTargetReleasesRegistrations();
        Test_S2_G13_NullCanvasRetryAndRereadAreNoOps();
        Test_S2_G14_RequestAfterCancelAllIsDelivered();
        Test_S2_G15_LatchBlockReleasesRegistrations();
        Test_S2_G16_JobLandedAndHasInFlightRequests();
        Test_S2_G17_HasInFlightFalseRightAfterCancelAll();
        Test_S2_G18_ReentrantSlotDetachesCanvas_NoCrash();
        Test_S2_G19_DedupMustNotSwallowRereadWhenInFlightReadAlreadyStarted();
        Test_S2_G20_SuccessfulVisibleRangeSurvivesPrefetchFailure();
        Test_S2_G21_LatchPathSignalIsDebouncedLikeGatePath();
        Test_S2_G22_PauseResumeDoesNotLeavePending();
        Test_S2_G23_SkippedRangeRequiresExplicitRetryAllFailed();

        if (ReproEnabled())
        {
            Test_S2_R7_NewRequestBeforeDirtyLandingMustNotReadScratchAgain();
            Test_S2_R8_DebounceResetsOnIdentityChange();
            Test_S2_R9_RereadSpanBeyondCacheStillCoversLastBlock();
        }
    }
}
