// HvmWatchEvidence：内存监视的证据侧实现。
//
// 与 HvmWatch.cpp 分开的理由不是行数，而是这两组东西回答的问题不同：
// HvmWatch.cpp 管的是"这张表里有哪些监视、怎么装怎么撤"，这里管的是"命中之后
// 还能问出什么"——去事件环取回那一行、把命中时刻的性能计数换算成人能读的时间、
// 以及把目标的人话标签存在 R3 这一侧（协议里没有标签字段）。
//
// 三者共同的前提是：**说不出来的事不要说**。取不回事件时要分清"从未命中"与
// "命中了但证据被挤出环"；换算不出墙钟时间时要报原始计数而不是一个看着干净的
// 时刻；标签的身份指纹对不上时宁可不显示，也不能把上一条监视的描述挂到另一个
// 目标上。

#include "HvmControl.h"

#include "../Internationalization/LanguageManager.h"

#include <QDateTime>
#include <QSettings>
#include <QString>

// 命中时间戳是 KeQueryPerformanceCounter 的计数值，换算成墙钟要 QPC 频率。
#include <Windows.h>

namespace ksword::hvm
{
    /*
     * findWatchHitEvent：去事件环里找这条 watch 最近一次命中的那一行。
     *
     * 入参 entry 是 watch 表快照里的一行；出参是四态结果，见头文件。
     *
     * 为什么要读环：watch 行里已经有 RIP / RSP / CR3 / GLA / GPA / 时间 / CPU，
     * 唯独没有 **qualification**（EPT violation 的退出限定符）——那个字段只存在
     * 于事件行里，而它正是"这次访问到底是读、是写、还是取指，以及线性地址有没有
     * 效"的原始读数。没有它，详情页只能转述驱动已经解释过的结论。
     *
     * 读法上用 lastHitSequence - 1 作游标而不是 0：环可能装着几万行，全量取回来
     * 再过滤是白搬一遍数据，而我们要的只是其中一行。游标用减一是因为 readEvents
     * 取的是**严格大于**该序号的行；lastHitSequence 为 0 的情况在前面已经被
     * NeverHit 拦掉，所以这里不会下溢。
     */
    HvmWatchHitEvent findWatchHitEvent(const HvmWatchEntry& entry)
    {
        HvmWatchHitEvent hit;
        /* 从未命中：这是一条结论，不需要也不应该去读环。 */
        if (entry.hitCount == 0UL ||
            entry.lastHitStatus == KSWORD_ARK_HVM_EPT_WATCH_HIT_NONE ||
            entry.lastHitSequence == 0ULL)
        {
            hit.kind = HvmWatchHitEventKind::NeverHit;
            hit.message = ks::i18n::sourceText(
                QStringLiteral("这条监视还没有命中过，所以没有现场事件可看。"));
            return hit;
        }
        /*
         * 命中那一刻就已经知道证据没发布成功。
         *
         * 驱动在命中路径里先把 lastHitStatus 记成 EVENT_LOST，发布成功才改写成
         * PUBLISHED。既然它自己说没发出去，就不必再去环里找一遍——找不到是必然的，
         * 而"找了一圈没找到"与"一开始就知道没发出去"在措辞上会被读成两件事。
         */
        if (entry.lastHitStatus == KSWORD_ARK_HVM_EPT_WATCH_HIT_EVENT_LOST)
        {
            hit.kind = HvmWatchHitEventKind::Evicted;
            hit.message = ks::i18n::sourceText(
                QStringLiteral("目标确实被访问过（命中 %1 次），但那一刻事件环没能接住这条记录，所以没有完整现场。监视自己保留的 RIP / RSP / CR3 仍然有效。"))
                .arg(entry.hitCount);
            return hit;
        }
        const HvmEventResult events =
            readEvents(entry.lastHitSequence - 1ULL, false);
        hit.newestSequence = events.newestSequence;
        hit.droppedRows = events.droppedRows;
        if (!events.ok)
        {
            /* 环没读出来。这与"目标没被访问"无关，必须分开说。 */
            hit.kind = HvmWatchHitEventKind::Unavailable;
            hit.message = ks::i18n::sourceText(
                QStringLiteral("读不到事件环，无法取回现场事件：%1"))
                .arg(events.message);
            return hit;
        }
        for (const HvmEventEntry& row : events.events)
        {
            /*
             * 序号是唯一判据。
             *
             * 不按 ruleId 匹配：同一条 watch 被重新武装以后会再次命中，环里
             * 就会有好几行共用同一个 ruleId，按它取会取到任意一行。序号是
             * 单调递增且唯一的，watch 行记下的就是那一次。
             */
            if (row.sequence != entry.lastHitSequence)
            {
                continue;
            }
            hit.kind = HvmWatchHitEventKind::Found;
            hit.event = row;
            hit.message = ks::i18n::sourceText(
                QStringLiteral("现场事件仍在环内。"));
            return hit;
        }
        /*
         * watch 说发布成功了，但环里已经没有那一行——它被后来的事件挤出去了。
         *
         * 这一态与 NeverHit 在界面上最容易长成同一句话，而结论正好相反：
         * 这里的事实是"目标被访问过、证据没留住"。
         */
        hit.kind = HvmWatchHitEventKind::Evicted;
        hit.message = ks::i18n::sourceText(
            QStringLiteral("命中事件（序号 %1）已经被后来的事件挤出事件环，取不回来了；环内当前最新序号是 %2。监视自己保留的现场仍然有效。"))
            .arg(entry.lastHitSequence)
            .arg(hit.newestSequence);
        return hit;
    }

    namespace
    {
        /*
         * 标签在注册表里的位置。
         *
         * 用默认构造的 QSettings：组织名与应用名已经在 main.cpp 里设过，
         * 别处的设置也都走这一条路径。
         */
        constexpr const char* kWatchLabelGroup = "HvmWatchLabels";

        /*
         * 身份指纹。
         *
         * watchId 是驱动分配的 ruleId，驱动重载后会从头开始发号。只按编号存
         * 标签，下一次装的另一条 watch 就会顶着上一条的描述显示出来——那不是
         * 缺了信息，是显示了一句错的，而且没有任何迹象提示它是错的。
         *
         * 指纹取四项：地址种类、请求地址、请求长度、实际监视页。它们在一条
         * watch 的整个生命周期里都不变（REARM 也不改），所以指纹稳定；而两条
         * 不同的 watch 要完全撞上这四项，那它们本来就是同一个目标。
         */
        QString watchFingerprint(const HvmWatchEntry& entry)
        {
            return QStringLiteral("%1:%2:%3:%4")
                .arg(entry.addressKind)
                .arg(entry.requestedAddress, 0, 16)
                .arg(entry.requestedLength, 0, 16)
                .arg(entry.physicalPage, 0, 16);
        }
    }

    void rememberWatchLabel(const HvmWatchEntry& entry, const QString& label)
    {
        if (entry.watchId == 0UL)
        {
            return;
        }
        if (label.isEmpty())
        {
            forgetWatchLabel(entry.watchId);
            return;
        }
        QSettings settings;
        settings.beginGroup(QLatin1String(kWatchLabelGroup));
        settings.beginGroup(QString::number(entry.watchId));
        settings.setValue(QStringLiteral("label"), label);
        settings.setValue(
            QStringLiteral("fingerprint"), watchFingerprint(entry));
        settings.endGroup();
        settings.endGroup();
    }

    QString watchLabel(const HvmWatchEntry& entry)
    {
        if (entry.watchId == 0UL)
        {
            return QString();
        }
        QSettings settings;
        settings.beginGroup(QLatin1String(kWatchLabelGroup));
        settings.beginGroup(QString::number(entry.watchId));
        const QString stored =
            settings.value(QStringLiteral("label")).toString();
        const QString fingerprint =
            settings.value(QStringLiteral("fingerprint")).toString();
        settings.endGroup();
        settings.endGroup();
        if (stored.isEmpty())
        {
            return QString();
        }
        /*
         * 指纹对不上就当没有标签。
         *
         * 这里宁可少显示一句人话，也不能把上一条 watch 的描述挂到一个不相干的
         * 目标上：那会让人拿着"DriverObject 的 dispatch 被改了"这个结论去查一段
         * 毫无关系的内存。
         */
        if (fingerprint != watchFingerprint(entry))
        {
            return QString();
        }
        return stored;
    }

    void forgetWatchLabel(const unsigned long watchId)
    {
        if (watchId == 0UL)
        {
            return;
        }
        QSettings settings;
        settings.beginGroup(QLatin1String(kWatchLabelGroup));
        settings.remove(QString::number(watchId));
        settings.endGroup();
    }

    void forgetAllWatchLabels()
    {
        QSettings settings;
        settings.remove(QLatin1String(kWatchLabelGroup));
    }

    QString describeWatchHitTime(const unsigned long long ticks)
    {
        if (ticks == 0ULL)
        {
            return ks::i18n::sourceText(QStringLiteral("未采集"));
        }
        LARGE_INTEGER frequency{};
        LARGE_INTEGER now{};
        // 频率为零或读不到时不猜：只把原始计数摆出来。
        if (QueryPerformanceFrequency(&frequency) == FALSE ||
            frequency.QuadPart <= 0 ||
            QueryPerformanceCounter(&now) == FALSE)
        {
            return ks::i18n::sourceText(
                QStringLiteral("性能计数 %1（本机读不到计数频率，换算不出墙钟时间）"))
                .arg(ticks);
        }
        const long long elapsedTicks =
            now.QuadPart - static_cast<long long>(ticks);
        /*
         * 差值为负说明命中"发生在未来"。
         *
         * 正常机器上不会出现，出现就说明计数源在两次读之间被重置过（睡眠、
         * 虚拟机迁移、计数源切换）。此时倒推出来的时间是错的，所以直接说
         * 换算不成立，而不是显示一个未来的时刻。
         */
        if (elapsedTicks < 0)
        {
            return ks::i18n::sourceText(
                QStringLiteral("性能计数 %1（比当前计数还大，说明计数源在这之间被重置过，换算不出可信的墙钟时间）"))
                .arg(ticks);
        }
        const long long elapsedMs =
            elapsedTicks * 1000LL / frequency.QuadPart;
        const QDateTime hitTime =
            QDateTime::currentDateTime().addMSecs(-elapsedMs);
        return ks::i18n::sourceText(
            QStringLiteral("%1（距今 %2 毫秒；由性能计数 %3 与当前时钟换算，不是命中当时读到的墙钟时间）"))
            .arg(hitTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
            .arg(elapsedMs)
            .arg(ticks);
    }
}
