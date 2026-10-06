#include "WorkbenchMessages.h"

#include "../../Internationalization/LanguageManager.h"

// ============================================================
// WorkbenchMessages.cpp
// 作用：见头文件。本文件是全部中文文案字面量的唯一落脚点；其余 WP-G 源文件
// 一律通过这里的函数取文案，不直接写 QStringLiteral 中文句子（除少量无法枚举、
// 纯粹拼装调用方已给定字符串的场景，例如直接回显进程名）。
//
// N2（第二轮修复）：本文件原来直接 return QStringLiteral(中文)，指望运行期的
// 全树翻译（LanguageManager 的 app 级 eventFilter）按控件当前文字做精确匹配
// 补上英文——这对两类消费者失效：① 自绘控件（HexViewSegmented/
// HexViewMessageLabel）不是标准 QLabel/QToolButton，扫描器结构上看不到它们的
// 文字；② 带 %1 占位符的模板，扫描器匹配的是 .arg() 替换之后的最终文本，而
// 模板字面量本身（含 %1）从不会等于任何一次渲染出来的整串，永远匹配不上。两种
// 情况下文字都会原样停在中文。改为在"文案唯一入口"这里用 ks::i18n::sourceText
// 包住每个固定字面量/模板（在 .arg() 替换之前），不管消费者是自绘控件还是标准
// 控件、字符串是不是参数化的，翻译在这里原地完成，不依赖后续任何运行期扫描；
// zh-CN（历史默认语言）下 sourceText 恒等透传，不影响中文界面。语言包里这批
// 键（进程/内核/物理/各通道说明/各 GateReason 文案/目标 chip 模板/CommitReport
// 两句）此前已经存在于 languages/{zh-CN,en-US}.json 的 source_translations
// （round 1 补的，但没有在这里真正调用过），这里只是让已经写好的翻译条目第一次
// 生效，没有新增任何语言包条目。
// ============================================================

namespace ks::ui::workbench_messages
{
    using ksword::memwb::ApprovalAnswer;
    using ksword::memwb::ApprovalRequest;
    using ksword::memwb::Channel;
    using ksword::memwb::CommitOutcome;
    using ksword::memwb::CommitReport;
    using ksword::memwb::ExprError;
    using ksword::memwb::GateReason;
    using ksword::memwb::GateVerdict;
    using ksword::memwb::IoReadStatus;
    using ksword::memwb::Issue;
    using ksword::memwb::ModeSwitchStatus;
    using ksword::memwb::PageState;
    using ksword::memwb::Scope;
    using ksword::memwb::SessionError;
    using ksword::memwb::StageStatus;
    using ksword::memwb::UiConfirmRequest;
    using ksword::memwb::WriteMode;

    // ChannelName：四个通道的短名，与会话条分段按钮文字一致。
    QString ChannelName(const Channel channel)
    {
        switch (channel)
        {
        case Channel::UserMode: return QStringLiteral("R3");
        case Channel::StandardDriver: return QStringLiteral("R0");
        case Channel::Hvm: return QStringLiteral("HVM");
        case Channel::Ddma: return QStringLiteral("DDMA");
        }
        return QStringLiteral("?");
    }

    // ScopeName：三个范围的短名。
    // N2：这三个短名会被喂进会话条的自绘分段控件（HexViewSegmented），运行期
    // 全树翻译扫不到自绘控件，这里改为源头直译。
    QString ScopeName(const Scope scope)
    {
        switch (scope)
        {
        case Scope::ProcessVirtual: return ks::i18n::sourceText(QStringLiteral("进程"));
        case Scope::KernelVirtual: return ks::i18n::sourceText(QStringLiteral("内核"));
        case Scope::Physical: return ks::i18n::sourceText(QStringLiteral("物理"));
        }
        return QStringLiteral("?");
    }

    // WriteModeName：写入模式的短名，供对话框正文引用。
    QString WriteModeName(const WriteMode mode)
    {
        return mode == WriteMode::Immediate
            ? QStringLiteral("立即写入")
            : QStringLiteral("暂存后应用");
    }

    // ChannelDescription：会话条通道分段按钮的常规提示，拆自旧 UiBuild.cpp:538 的长说明。
    // N2：这四句是自绘分段控件（HexViewSegmented）的悬停提示文字，同样经源头直译。
    QString ChannelDescription(const Channel channel)
    {
        switch (channel)
        {
        case Channel::UserMode:
            return ks::i18n::sourceText(QStringLiteral("用户态 API 通道（R3）：走系统公开的进程内存接口，只能读写已附加的进程，权限受目标进程保护属性限制。"));
        case Channel::StandardDriver:
            return ks::i18n::sourceText(QStringLiteral("标准驱动通道（R0）：经 KswordARK 内核驱动读写，可访问内核与物理地址，受驱动是否已加载限制。"));
        case Channel::Hvm:
            return ks::i18n::sourceText(QStringLiteral("虚拟化监视器通道（R-1）：经常驻的虚拟化监视器读写，权限最高，首次使用前需要一次异步可用性探测。"));
        case Channel::Ddma:
            return ks::i18n::sourceText(QStringLiteral("磁盘 DMA 通道：借磁盘传输暂存扇区中转读写，不依赖目标进程权限，但依赖磁盘传输会话已配置就绪。"));
        }
        return QString();
    }

    // ChannelUnavailableReason：按 GateReason 翻译"为什么不可用/为什么还不确定"。
    QString ChannelUnavailableReason(const Channel channel, const GateVerdict verdict)
    {
        if (verdict.available && !verdict.IsUnknown())
        {
            return QString();
        }
        // N2：这句话最终会喂进会话条的自绘通道分段/自绘 HexViewMessageLabel
        // 提示条，两者都不是标准 QLabel/QToolButton，运行期全树翻译扫不到；加上
        // 带 %1 的模板本身也永远不会等于替换后的最终文本（扫描器按整串精确匹配），
        // 这里统一在模板上先做 sourceText，再 .arg()。
        switch (verdict.reason)
        {
        case GateReason::None:
            return QString();
        case GateReason::ScopeNotSupported:
            return ks::i18n::sourceText(QStringLiteral("%1 只支持进程范围，当前范围下不可用。")).arg(ChannelName(channel));
        case GateReason::NeedsPid:
            return ks::i18n::sourceText(QStringLiteral("还没有目标进程，进程范围下的通道暂不可用。"));
        case GateReason::DriverNotLoaded:
            return ks::i18n::sourceText(QStringLiteral("KswordARK 驱动未加载，%1 暂不可用。")).arg(ChannelName(channel));
        case GateReason::ProbeNotDone:
            return ks::i18n::sourceText(QStringLiteral("%1 的可用性正在后台探测，结果出来前仍可选用。")).arg(ChannelName(channel));
        case GateReason::ProbeFailed:
            return ks::i18n::sourceText(QStringLiteral("%1 的可用性探测已完成，结论是当前不可用。")).arg(ChannelName(channel));
        case GateReason::SessionNotReady:
            return ks::i18n::sourceText(QStringLiteral("磁盘传输会话尚未配置就绪，DDMA 暂不可用。"));
        case GateReason::InvalidRequest:
            return ks::i18n::sourceText(QStringLiteral("当前范围/通道组合无效。"));
        }
        // B11：GateReason 越界（未来新增枚举值但本文件漏改）时绝不能吞成空串——
        // 红色提示条会被置成可见但没有一个字的空壳，用户完全看不出发生了什么。
        // 带上原始数值，至少能让人反馈"原因代码 N 是什么"。
        return ks::i18n::sourceText(QStringLiteral("该通道当前不可用（原因代码 %1）。")).arg(static_cast<quint32>(verdict.reason));
    }

    // Translate(CommitOutcome)：提交结果的中文短句，供状态条"写入结果"段使用。
    QString Translate(const CommitOutcome outcome)
    {
        switch (outcome)
        {
        case CommitOutcome::NoChange: return QStringLiteral("没有暂存的修改，未做任何写入。");
        case CommitOutcome::Committed: return QStringLiteral("已写入并回读确认。");
        case CommitOutcome::UserCancelled: return QStringLiteral("用户取消了写入确认。");
        case CommitOutcome::Stale: return QStringLiteral("确认期间目标发生了变化，写入已取消，请重新读取。");
        case CommitOutcome::TargetChanged: return QStringLiteral("目标字节与暂存时的原值不一致，未写入，请重新读取。");
        case CommitOutcome::ApprovalDenied: return QStringLiteral("用户拒绝了驱动要求的强制同意，写入已取消。");
        case CommitOutcome::WriteFailed: return QStringLiteral("写入失败。");
        case CommitOutcome::VerifyMismatch: return QStringLiteral("写入后回读与预期不一致，可能只写入了部分字节。");
        case CommitOutcome::InvalidSession: return QStringLiteral("当前目标会话不自洽，未尝试写入。");
        case CommitOutcome::Busy: return QStringLiteral("已有一次写入在进行，本次请求被忽略。");
        }
        return QStringLiteral("未知的写入结果。");
    }

    // Translate(ApprovalAnswer)：目前仅用于日志/诊断文本，不在界面上单独展示按钮以外的文字。
    QString Translate(const ApprovalAnswer answer)
    {
        switch (answer)
        {
        case ApprovalAnswer::Deny: return QStringLiteral("拒绝");
        case ApprovalAnswer::ThisBlockOnly: return QStringLiteral("仅此块强制");
        case ApprovalAnswer::RestOfBatch: return QStringLiteral("本次其余块也强制");
        }
        return QStringLiteral("未知");
    }

    // Translate(ExprError)：地址表达式求值失败原因，供地址条红边提示使用（WP-I 消费）。
    QString Translate(const ExprError error)
    {
        switch (error)
        {
        case ExprError::None: return QString();
        case ExprError::Empty: return QStringLiteral("请输入地址或表达式。");
        case ExprError::BadNumber: return QStringLiteral("数字格式不对，请检查进制前缀与数位。");
        case ExprError::Overflow: return QStringLiteral("数值超出 64 位范围。");
        case ExprError::UnknownModule: return QStringLiteral("找不到这个模块。");
        case ExprError::AmbiguousModule: return QStringLiteral("模块名重名，请写完整路径。");
        case ExprError::NeedsProcess: return QStringLiteral("这个表达式需要一个进程上下文。");
        case ExprError::BadSyntax: return QStringLiteral("表达式写法不对，请检查括号与运算符。");
        case ExprError::DerefFailed: return QStringLiteral("解引用读取失败。");
        }
        return QStringLiteral("表达式无法解析。");
    }

    // Translate(Issue)：地址解析细分成因，供地址条/导航提示使用（WP-I 消费）。
    QString Translate(const Issue issue)
    {
        switch (issue)
        {
        case Issue::None: return QString();
        case Issue::NoTarget: return QStringLiteral("还没有目标，无法解析地址。");
        case Issue::ScopeHasNoModules: return QStringLiteral("当前范围没有模块可供按名查找。");
        case Issue::ModulesNotLoaded: return QStringLiteral("模块列表尚未加载。");
        case Issue::ModulesLoading: return QStringLiteral("模块列表正在加载，请稍候。");
        case Issue::ModulesOwnerMismatch: return QStringLiteral("模块列表属于另一个目标，已失效。");
        case Issue::NotFound: return QStringLiteral("找不到这个模块。");
        case Issue::FoundInKernelOnly: return QStringLiteral("只在内核模块里找到，可切换到内核范围后跳转。");
        case Issue::Ambiguous: return QStringLiteral("模块名重名，请写完整路径。");
        case Issue::DerefDeniedScope: return QStringLiteral("当前范围不支持解引用。");
        case Issue::DerefDeniedDdma: return QStringLiteral("DDMA 通道不支持解引用。");
        case Issue::DerefReadFailed: return QStringLiteral("解引用读取失败。");
        }
        return QStringLiteral("地址无法解析。");
    }

    // Translate(PageState)：旧称"IoReadStatus"，供状态条"已读 M/N 字节"段与画布占位符说明使用。
    QString Translate(const PageState state)
    {
        switch (state)
        {
        case PageState::Valid: return QStringLiteral("已读到真实数据。");
        case PageState::PartiallyValid: return QStringLiteral("只读到部分字节，其余不可读。");
        case PageState::Unreadable: return QStringLiteral("已尝试读取但不可读。");
        case PageState::NotAttempted: return QStringLiteral("尚未尝试读取。");
        }
        return QStringLiteral("未知读取状态。");
    }

    // Translate(IoReadStatus)：B11——MemoryIoPort.h 的 IoReadStatus 是读通道的四种
    // 事实（不是四级失败），Failed（通道自身失败，可重试，不代表目标不可读）与
    // Unreadable（目标本身读不到，换通道也一样）的文案必须明显不同，不能共用
    // PageState 的句子（那是另一个枚举，语义不等价）。
    QString Translate(const IoReadStatus status)
    {
        switch (status)
        {
        case IoReadStatus::Ok: return QStringLiteral("已读到全部请求的字节。");
        case IoReadStatus::Partial: return QStringLiteral("只读到一部分字节，其余本次没有读到。");
        case IoReadStatus::Unreadable: return QStringLiteral("目标当前不可读（原因在目标本身，换通道也不会改变）。");
        case IoReadStatus::Failed: return QStringLiteral("通道自身访问失败，无法判断目标是否可读；可以重试或检查通道状态。");
        }
        return QStringLiteral("未知的读取状态。");
    }

    // Translate(SessionError)：Validate(session) 失败时的具体违规说明。
    QString Translate(const SessionError error)
    {
        switch (error)
        {
        case SessionError::None: return QString();
        case SessionError::NeedsPid: return QStringLiteral("进程范围需要一个目标进程号，当前会话还没有。");
        case SessionError::PidMustBeZero: return QStringLiteral("内核或物理范围不该携带进程号，当前会话却带了。");
        case SessionError::BadAddressBits: return QStringLiteral("地址位宽既不是 32 位也不是 64 位。");
        case SessionError::BadScope: return QStringLiteral("范围取值不是进程、内核或物理三者之一。");
        case SessionError::BadChannel: return QStringLiteral("通道取值不是 R3/R0/HVM/DDMA 四者之一。");
        }
        return QStringLiteral("当前目标会话不自洽（未知原因）。");
    }

    // Translate(ModeSwitchStatus)：SetMode / ResolveModeSwitch 的结果短句。
    QString Translate(const ModeSwitchStatus status)
    {
        switch (status)
        {
        case ModeSwitchStatus::Switched: return QStringLiteral("写入模式已切换。");
        case ModeSwitchStatus::NeedsDecision: return QStringLiteral("还有未提交的修改，需要先决定如何处理。");
        case ModeSwitchStatus::Cancelled: return QStringLiteral("已取消本次切换，写入模式未改变。");
        case ModeSwitchStatus::ApplyFailed: return QStringLiteral("切换前应用未提交的修改失败，写入模式未改变。");
        case ModeSwitchStatus::NoPendingSwitch: return QStringLiteral("没有待决的切换请求。");
        case ModeSwitchStatus::Busy: return QStringLiteral("已有一次写入在进行，本次切换被忽略。");
        }
        return QStringLiteral("未知的模式切换结果。");
    }

    // Translate(StageStatus)：MemoryDiffOverlay::Stage 的拒绝原因，每种拒绝对用户
    // 来说都应该是"该怎么改"的提示，不是笼统的"暂存失败"。
    QString Translate(const StageStatus status)
    {
        switch (status)
        {
        case StageStatus::Ok: return QStringLiteral("已暂存。");
        case StageStatus::Empty: return QStringLiteral("没有给出任何字节，无法暂存。");
        case StageStatus::AddressOverflow: return QStringLiteral("地址加长度超出了可表示的范围。");
        case StageStatus::OutOfWindow: return QStringLiteral("这段范围没有完整落在当前已读取的窗口内，请先重新读取。");
        case StageStatus::UnreadBytes: return QStringLiteral("这段范围里含有从未真实读到的字节，不能暂存。");
        case StageStatus::TooLarge: return QStringLiteral("暂存总量会超过上限，请先应用或丢弃已有的修改。");
        }
        return QStringLiteral("未知的暂存结果。");
    }

    // CommitReportSummary：拼"写入结果"段的主句（块数/字节数随 outcome 附加说明）。
    // B9：条件改成"字节数或块数任一非零"——VerifyMismatch/WriteFailed 的部分写入
    // 也可能在目标上留下了字节（CommitReport::bytesWritten 的定义本来就含"回读未
    // 通过的块与部分写入"），绝不能让用户以为什么都没发生。Committed 分支单独拼
    // 字，避免和 Translate(Committed) 里已经出现过的"已写入"重复两次。
    QString CommitReportSummary(const CommitReport& report)
    {
        // N2：这两个模板带 %1/%2 占位符，拼接后的整串从不等于模板字面量本身，
        // 运行期按整串精确匹配的翻译永远命中不了——这里改成先对模板本身调用
        // sourceText，再 .arg() 填数字。
        QString text = Translate(report.outcome);
        if (report.outcome == CommitOutcome::Committed)
        {
            text += ks::i18n::sourceText(QStringLiteral("共 %1 字节（%2 块）。"))
                .arg(QString::number(static_cast<qulonglong>(report.bytesWritten)),
                     QString::number(static_cast<qulonglong>(report.blocksWritten)));
        }
        else if (report.bytesWritten > 0 || report.blocksWritten > 0)
        {
            text += ks::i18n::sourceText(QStringLiteral(" 已写入 %1 字节（%2 块）。"))
                .arg(QString::number(static_cast<qulonglong>(report.bytesWritten)),
                     QString::number(static_cast<qulonglong>(report.blocksWritten)));
        }
        if (!report.failureText.empty())
        {
            text += QStringLiteral(" (%1)").arg(QString::fromStdString(report.failureText));
        }
        return text;
    }

    // ExplainReadFailure：按旧诊断逻辑的输出顺序拼句——低地址保护区在前（与目标无关），
    // 然后是通道自身失败说明，最后是区域查询结果；MEM_FREE 时追加同名进程提示。
    QString ExplainReadFailure(const ReadFailureContext& context)
    {
        QString guardText;
        if (context.isLowAddressGuard)
        {
            guardText = QStringLiteral(
                "该地址落在进程的空指针保护区（低 64 KB）内，任何进程都不会在这里映射内存，"
                "请检查地址是否写少了位数。");
        }

        QString regionText;
        if (context.regionQueryOk)
        {
            regionText = context.regionSummary;
            if (context.regionIsFree)
            {
                regionText += QStringLiteral("。该地址在这个进程里根本没有内存（MEM_FREE），不是读不到——换任何通道都一样");
                if (context.sameNameProcessCount > 1)
                {
                    // processName 同样用户可控，与上面 TargetChipAttachedText 一致地用
                    // 单次多参数 .arg() 调用，不链式替换。
                    regionText += QStringLiteral("。本机有 %1 个都叫 %2 的进程，地址很可能属于其中另一个，请核对 PID")
                        .arg(QString::number(context.sameNameProcessCount), context.processName);
                }
            }
        }
        else
        {
            regionText = QStringLiteral("区域查询也失败，该地址在本进程中不存在");
        }

        // S6：rawFailureText（通道自身返回的原始说明，可能含英文/错误码，不受本文件
        // 控制）与 regionText（内部可能拼入 context.processName，同样用户可控）都是
        // 不信任内容。六个值放进同一次 .arg() 调用里原子替换，避免早替换的值里字面
        // 出现的占位符被后面的链式调用误吃掉（同一类问题见 TargetChipAttachedText）。
        return QStringLiteral("%1 读取失败：地址=0x%2（PID %3）。%4%5。%6")
            .arg(context.channelName, QString::number(context.address, 16), QString::number(context.pid),
                 guardText, context.rawFailureText, regionText);
    }

    // —— 会话条 ——

    QString TargetChipAttachedText(
        const QString& processName,
        const quint32 pid,
        const quint32 addressBits,
        const bool canReadWrite)
    {
        // S6：processName 是用户可控字符串（进程可以叫任何名字，含字面的 "%2" 也
        // 合法）。四个值必须在同一次 .arg() 调用里原子替换——链式 .arg().arg()...
        // 会在每一步重新扫描整条已替换文本找"最小的占位符"，processName 里字面
        // 出现的 "%2"/"%3" 就会被后续调用误当成真正的占位符吃掉。
        // N2：模板与"可读写/只读"都经 sourceText 源头直译（这个 chip 是标准
        // QToolButton，但整串文字因为带 processName/pid 这些可变参数，从不会等于
        // 任何固定的翻译键，运行期扫描同样命中不了）；processName 是进程自己的
        // 名字，绝不翻译，原样塞进去。
        return ks::i18n::sourceText(QStringLiteral("%1 · PID %2 · x%3 · %4"))
            .arg(processName, QString::number(pid), QString::number(addressBits),
                 canReadWrite ? ks::i18n::sourceText(QStringLiteral("可读写"))
                              : ks::i18n::sourceText(QStringLiteral("只读")));
    }

    QString TargetChipUnattachedText()
    {
        return QStringLiteral("未附加进程 — 点击选择");
    }

    QString TargetChipNoProcessText(const Scope scope)
    {
        // B3：内核/物理范围不需要任何进程，不该沿用"未附加进程"的红字警告——那会让
        // 用户以为自己忘了选目标，实际上这个范围压根不存在"选目标"这一步。
        switch (scope)
        {
        case Scope::KernelVirtual: return QStringLiteral("无需进程（内核）");
        case Scope::Physical: return QStringLiteral("无需进程（物理）");
        case Scope::ProcessVirtual: break;
        }
        return QString();
    }

    QString TargetChipTooltip(const bool needsProcess)
    {
        if (!needsProcess)
        {
            // S-e（第二轮修复）：内核/物理范围没有"选择进程"这一步，提示语不该
            // 还暗示点它能选进程——那会让用户以为自己漏做了什么。
            return ks::i18n::sourceText(QStringLiteral("当前范围不需要选择进程"));
        }
        return ks::i18n::sourceText(QStringLiteral("当前目标；点击在上方进程栏选择或附加"));
    }

    QString WriteModeTooltip()
    {
        return QStringLiteral(
            "写入模式（Ctrl+E）。立即写入：改完马上写入目标，写前复核原值、写后回读确认；"
            "暂存后应用：修改先只留在本窗口（橙色），点 ✓ 才一次写入。");
    }

    QString PendingPatchesText(const quint64 bytesPending, const quint64 blocksPending)
    {
        return QStringLiteral("%1 字节待写入（%2 处）")
            .arg(static_cast<qulonglong>(bytesPending))
            .arg(static_cast<qulonglong>(blocksPending));
    }

    QString ApplyButtonTooltip()
    {
        return QStringLiteral("把待写入修改写入目标（Ctrl+Enter），先复核原值、写后回读");
    }

    QString DiscardButtonTooltip()
    {
        return QStringLiteral("丢弃待写入修改，目标内存不受影响");
    }

    // —— 写入模式三选一 ——

    QString ModeSwitchDialogTitle()
    {
        return QStringLiteral("切换写入模式");
    }

    QString ModeSwitchDialogBody(const quint64 pendingBytes, const quint64 pendingBlocks, const WriteMode toMode)
    {
        return QStringLiteral("还有 %1 个字节（%2 处）的修改尚未写入目标。切换到「%3」前必须先处理：")
            .arg(static_cast<qulonglong>(pendingBytes))
            .arg(static_cast<qulonglong>(pendingBlocks))
            .arg(WriteModeName(toMode));
    }

    QString ApplyThenSwitchButtonText()
    {
        return QStringLiteral("应用并切换");
    }

    QString DiscardThenSwitchButtonText()
    {
        return QStringLiteral("丢弃并切换");
    }

    QString ModeSwitchCancelButtonText()
    {
        return QStringLiteral("取消");
    }

    QString ApplyFailedDiagnosticsPrefix()
    {
        return QStringLiteral("切换前应用待写入修改失败，模式未切换：");
    }

    // —— 离开前存在未提交的暂存补丁（修复缺陷 3，见头文件声明处的注释）——

    QString LeaveWithPendingDialogTitle()
    {
        return QStringLiteral("有未提交的修改");
    }

    QString LeaveWithPendingDialogBody(
        const quint64 pendingBytes, const quint64 pendingBlocks, const QString& reasonText)
    {
        // 与 ModeSwitchDialogBody 同一写法：pendingBytes/pendingBlocks 是本类自己
        // 算出的计数，不含用户数据，链式 .arg() 不会把数字内容误当占位符；
        // reasonText 固定是调用方几个预设短句之一，最后单独一次 .arg() 替换。
        return QStringLiteral(
            "还有 %1 个字节（%2 处）的修改尚未写入目标。%3前必须先处理：")
            .arg(static_cast<qulonglong>(pendingBytes))
            .arg(static_cast<qulonglong>(pendingBlocks))
            .arg(reasonText);
    }

    QString LeaveWithPendingApplyButtonText()
    {
        return QStringLiteral("应用并离开");
    }

    QString LeaveWithPendingDiscardButtonText()
    {
        return QStringLiteral("丢弃并离开");
    }

    QString LeaveWithPendingCancelButtonText()
    {
        return QStringLiteral("取消");
    }

    // —— 普通确认 ——

    QString UiConfirmTitle()
    {
        return QStringLiteral("应用内存修改");
    }

    QString UiConfirmBody(
        const UiConfirmRequest& request,
        const Scope scope,
        const Channel channel,
        const QString& targetDescription)
    {
        // B2：正文必须写明"通道 · 范围 · 目标"，不得把 request.targetIdentity 这种
        // 机器可读的 IdentityKey（形如 memwb-target/1|scope=0|pid=1234|...）原样
        // 显示给用户——内核/物理写入是最危险的确认，用户得看出自己在经哪条通道写
        // 哪里，而不是对着一串 "|" 分隔的键发呆。targetDescription 由调用方给出
        // （生产环境取会话条目标 chip 的"进程名 · PID · 位数"文案），空串时退回
        // 保守说法"当前目标"。
        // 沿用旧风险句（DriverMemoryRw.cpp 约 1106-1109 行）本身的措辞，只是把目标
        // 换成人话、并把通道/范围前缀上去。
        const QString target = targetDescription.isEmpty() ? QStringLiteral("当前目标") : targetDescription;
        // S6：target 可能含用户可控的进程名（字面出现 "%2" 之类也合法），五个值必须
        // 在同一次 .arg() 调用里原子替换，不能链式 .arg().arg()...。
        return QStringLiteral(
            "通道：%1 · 范围：%2\n"
            "将写入 %3 个差异块（共 %4 字节）到 %5。\n"
            "内核或进程内存修改可能立即造成数据损坏、权限边界失效、进程崩溃或系统蓝屏。\n"
            "只写入和原始备份不同的字节，是否继续？")
            .arg(ChannelName(channel), ScopeName(scope),
                 QString::number(static_cast<qulonglong>(request.blocksTotal)),
                 QString::number(static_cast<qulonglong>(request.bytesTotal)), target);
    }

    QString DontAskAgainThisRunText()
    {
        return QStringLiteral("本次运行不再询问");
    }

    QString UiConfirmAcceptButtonText()
    {
        return QStringLiteral("写入");
    }

    QString UiConfirmRejectButtonText()
    {
        return QStringLiteral("取消");
    }

    // —— 强制同意 ——

    QString ApprovalDialogTitle()
    {
        return QStringLiteral("强制写入确认");
    }

    QString ApprovalDialogBody(
        const ApprovalRequest& request,
        const Scope scope,
        const Channel channel,
        const QString& targetDescription)
    {
        // B2：同 UiConfirmBody，不展示原始 IdentityKey，改展示通道/范围/人话目标。
        // 沿用旧强制确认句式（DriverMemoryRw.cpp 约 1813-1820 行）本身的措辞。
        const QString target = targetDescription.isEmpty() ? QStringLiteral("当前目标") : targetDescription;
        // S6：八个值必须在**同一次** .arg() 调用里原子替换——哪怕拆成两次各四个的
        // 多参数调用，第一批里 target（用户可控）一旦字面包含 "%5".."%8"，第二批
        // 调用扫描整条当前字符串时仍会把它误当占位符吃掉。Qt 的多参数 arg() 对
        // string-like 类型没有参数个数上限，一次给全最安全。
        return QStringLiteral(
            "通道：%1 · 范围：%2\n"
            "目标：%3\n块 %4/%5　起始地址=0x%6　长度=%7 字节\n\n%8\n\n"
            "强制继续会绕过这一次写入保护，只应在确认目标与地址无误时使用。")
            .arg(ChannelName(channel), ScopeName(scope), target,
                 QString::number(static_cast<qulonglong>(request.blockIndex) + 1),
                 QString::number(static_cast<qulonglong>(request.blocksTotal)),
                 QString::number(request.address, 16),
                 QString::number(static_cast<qulonglong>(request.length)),
                 QString::fromStdString(request.backendText));
    }

    QString ApprovalThisBlockOnlyButtonText()
    {
        return QStringLiteral("仅此块强制");
    }

    QString ApprovalRestOfBatchButtonText()
    {
        return QStringLiteral("本次其余块也强制");
    }

    QString ApprovalCancelButtonText()
    {
        return QStringLiteral("取消");
    }

    // —— 状态条常驻/瞬时 chip ——

    QString ScratchAreaDirtyChipText()
    {
        return QStringLiteral("暂存扇区未还原");
    }

    QString ScratchAreaDirtyAckTooltip()
    {
        return QStringLiteral("我已知晓：暂存扇区未还原");
    }

    QString ReadModifyWriteWindowChipText()
    {
        return QStringLiteral("读-改-写窗口，同页其他字节写入期间可能被覆盖");
    }

    QString NeedsRereadHintText()
    {
        return QStringLiteral("目标可能已变化，建议重新读取");
    }

    QString CopyDiagnosticsButtonTooltip()
    {
        return QStringLiteral("复制诊断文本");
    }

    QString ExpandDiagnosticsTooltip(const bool expanded)
    {
        return expanded ? QStringLiteral("收起诊断") : QStringLiteral("展开完整诊断");
    }

    QString DiagnosticsWrapCheckboxText()
    {
        return QStringLiteral("自动换行");
    }

    // —— 字符串写入对话框 ——

    QString StringWriteDialogTitle()
    {
        return QStringLiteral("写入字符串");
    }

    QString StringWriteEncodingLabel(const int encodingIndex)
    {
        switch (encodingIndex)
        {
        case 0: return QStringLiteral("ANSI");
        case 1: return QStringLiteral("UTF-8");
        case 2: return QStringLiteral("UTF-16LE");
        default: return QString();
        }
    }

    QString StringWriteNulCheckboxText()
    {
        return QStringLiteral("末尾写入 NUL 结束符");
    }

    QString StringWriteInputPlaceholder()
    {
        return QStringLiteral("要写入的文本…");
    }

    QString StringWritePreviewText(const quint64 byteCount)
    {
        return QStringLiteral("将写入 %1 字节").arg(static_cast<qulonglong>(byteCount));
    }

    QString StringWriteAnsiLossyText()
    {
        return QStringLiteral("含无法用 ANSI 表示的字符");
    }

    QString StringWriteOkButtonText()
    {
        return QStringLiteral("写入");
    }

    QString StringWriteCancelButtonText()
    {
        return QStringLiteral("取消");
    }
}
