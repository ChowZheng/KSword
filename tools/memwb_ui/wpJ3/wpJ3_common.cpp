// wpJ3_common.cpp
// 作用：wpJ3_common.h 声明的公共设施的实现——断言计数、画布装配/填页小工具、
// 事件泵、Feeder 信号录像机。

#include "wpJ3_common.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cstdio>

namespace memwb_wpJ3_test
{
    // g_checks / g_failures：全部断言的累计计数，main.cpp 结束时打印汇总行。
    int g_checks = 0;
    int g_failures = 0;

    // Report：累加计数，失败时把表达式、位置与说明打到 stderr，方便定位哪一行断言
    // 没通过（不中断测试，其它断言照常继续跑，尽量一次性暴露所有问题）。
    void Report(bool ok, const char* expression, const char* file, int line, const QString& note)
    {
        ++g_checks;
        if (ok)
        {
            return;
        }
        ++g_failures;
        if (note.isEmpty())
        {
            std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        }
        else
        {
            std::fprintf(stderr, "FAIL %s:%d: %s (%s)\n", file, line, expression, note.toUtf8().constData());
        }
    }

    // SetupCanvas：见头文件注释。顺序固定：先挂提供者再设地址空间，这样首次
    // requestVisiblePages 发生时提供者已经就位。
    void SetupCanvas(
        HexCanvas& canvas,
        NoOpPageProvider& provider,
        std::uint64_t firstAddress,
        std::uint64_t lastAddress)
    {
        canvas.setPageProvider(&provider);
        canvas.setAddressSpace(firstAddress, lastAddress);
    }

    // DeliverValidPage：构造一页全 1 掩码的数据并喂给画布，来源代次用画布当前的
    // sourceRevision()（换目标/重读之后旧代次会被 HexViewport 拒收，测试里必须跟
    // 当前代次保持一致才能让 deliverPage 真正生效）。
    HexCanvas::PageResult DeliverValidPage(HexCanvas& canvas, std::uint64_t pageStart, std::uint8_t fillValue)
    {
        QByteArray bytes(static_cast<int>(kPageBytes), static_cast<char>(fillValue));
        QByteArray mask(static_cast<int>(kPageBytes), static_cast<char>(1));
        return canvas.deliverPage(pageStart, bytes, mask, canvas.sourceRevision());
    }

    // DeliverPartialPage：前 validPrefixBytes 字节掩码为 1，其余为 0；整页仍然算
    // "已缓存"（落定），只是部分字节的状态是 Unreadable 而不是 Valid。
    HexCanvas::PageResult DeliverPartialPage(
        HexCanvas& canvas,
        std::uint64_t pageStart,
        std::uint8_t fillValue,
        std::size_t validPrefixBytes)
    {
        QByteArray bytes(static_cast<int>(kPageBytes), static_cast<char>(fillValue));
        QByteArray mask(static_cast<int>(kPageBytes), static_cast<char>(0));
        const std::size_t clamped = validPrefixBytes > kPageBytes ? static_cast<std::size_t>(kPageBytes) : validPrefixBytes;
        for (std::size_t index = 0; index < clamped; ++index)
        {
            mask[static_cast<int>(index)] = static_cast<char>(1);
        }
        return canvas.deliverPage(pageStart, bytes, mask, canvas.sourceRevision());
    }

    // DeliverUnreadablePage：整页标不可读，走 deliverUnreadable（区别于"部分有效"：
    // 这页从未成功读到任何一个字节）。
    HexCanvas::PageResult DeliverUnreadablePage(HexCanvas& canvas, std::uint64_t pageStart)
    {
        ks::ui::HexFetchRange range;
        range.firstPageStart = pageStart;
        range.pageCount = 1;
        return canvas.deliverUnreadable(range, canvas.sourceRevision());
    }

    // DeliverCustomPage：整页全部有效（掩码全 1），内容由调用方给定，供需要精确
    // 控制"哪些字节变了、哪些没变"的测试使用。
    HexCanvas::PageResult DeliverCustomPage(HexCanvas& canvas, std::uint64_t pageStart, const QByteArray& bytes)
    {
        if (static_cast<std::uint64_t>(bytes.size()) != kPageBytes)
        {
            return HexCanvas::PageResult::RejectedBadSize;
        }
        QByteArray mask(static_cast<int>(kPageBytes), static_cast<char>(1));
        return canvas.deliverPage(pageStart, bytes, mask, canvas.sourceRevision());
    }

    // PumpUntil：每轮处理一次事件循环（含定时器到期回调），predicate 成立就立即
    // 返回；否则小睡 1 ms 再试，直到总耗时超过 timeoutMs。
    bool PumpUntil(const std::function<bool()>& predicate, int timeoutMs)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate())
        {
            if (timer.elapsed() >= timeoutMs)
            {
                return predicate();
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        return true;
    }

    // BaselineRecorder::Attach：lambda 捕获 this（录像机实例），连接到 context 的
    // 生命周期——context 销毁时 Qt 自动断开连接，录像机本身不需要手动 disconnect。
    void BaselineRecorder::Attach(ks::ui::WorkbenchBaselineFeeder& feeder, QObject& context)
    {
        QObject::connect(
            &feeder,
            &ks::ui::WorkbenchBaselineFeeder::baselineRefreshed,
            &context,
            [this](quint64 base, quint64 length)
            {
                ++refreshedCount;
                lastBase = base;
                lastLength = length;
            });
        QObject::connect(
            &feeder,
            &ks::ui::WorkbenchBaselineFeeder::baselineUnavailable,
            &context,
            [this](ksword::memwb::BaselineSpanStatus status)
            {
                ++unavailableCount;
                lastStatus = status;
            });
    }

    // Reset：只清计数与记录值，信号连接保持不变（测试里常见"跑一段场景、清一次
    // 计数、再跑下一段场景"的写法，不需要重新 Attach）。
    void BaselineRecorder::Reset()
    {
        refreshedCount = 0;
        unavailableCount = 0;
        lastBase = 0;
        lastLength = 0;
        lastStatus = ksword::memwb::BaselineSpanStatus::Ok;
    }
}
