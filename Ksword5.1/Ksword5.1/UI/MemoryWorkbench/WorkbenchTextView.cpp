// WorkbenchTextView.cpp
// 作用：WorkbenchTextView 的界面搭建与数据流，以及三种编码 + validMask 联动的解码实现
// （DecodeTextChunkForTest，供夹具直接测试，不依赖界面）。

#include "WorkbenchTextView.h"

#include "HexCanvasFormat.h"
#include "../CodeEditorWidget.h"

#include "../../theme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace ks::ui
{
    namespace
    {
        // kUnreadableGlyph：不可读字节（没有被 FetchWindow 读到）的占位字符，与 HexCanvasFormat
        // 的乘号约定一致。
        const QChar kUnreadableGlyph(hexcanvas_format::kUnreadableAsciiGlyph);

        // kInvalidUtf8SequenceGlyph：D11——UTF-8 路径专用的第二个占位符，表示"这个位置的字节
        // 确确实实被读到了，只是取值不构成合法的 UTF-8 序列"，用 U+FFFD（Unicode 官方的
        // "替换字符"，本身语义就是"这里曾经有一个解码失败的字符"）跟 kUnreadableGlyph 区分，
        // 不再像旧代码一样把"读到了但是乱码"和"压根没读到"画成同一个乘号——不变式 5/6 要求
        // 这两类必须能分开看。只在本文件内部使用，不导出，不影响 ANSI/UTF-16 两条路径
        // （它们本来就没有"合法性"这个维度，只有可读/不可读、可打印/不可打印两个维度）。
        constexpr char16_t kInvalidUtf8SequenceGlyph = 0xFFFDU;

        // formatHexDigitsUpper：D4——只把地址的十六进制数字部分转大写，不带 "0x" 前缀，
        // 不对整条状态栏模板字符串调用 toUpper()（那样会把模板里的英文字面量也带着一起
        // 转大写，破坏 LanguageManager 运行时翻译用的大小写敏感模板匹配）。与
        // WorkbenchDisasmView.cpp 里的同名函数各自一份——两个 .cpp 是不同的编译单元，
        // 这个小写死的纯函数不值得为了复用拉一个新的共享头文件。
        QString formatHexDigitsUpper(const quint64 value, const int width)
        {
            return QString::number(value, 16).rightJustified(width, QChar('0')).toUpper();
        }

        // isDisplayableChar：在 QChar::isPrint() 基础上再排除 Zl/Zp/Cf 三个 Unicode 类别。
        // D11：isPrint() 认为 U+2028(行分隔符)/U+2029(段分隔符) 是"可打印"的，但
        // CodeEditorWidget 把它们当成真正的换行处理，会把本该与十六进制页对齐的一整
        // 逻辑行拆成两个视觉行；Cf（格式字符，例如零宽连接符）本身不占可见宽度，显示出来
        // 既看不见也数不出字符，两者都退化成点号，与"看不出字符"的既有惯例一致。
        bool isDisplayableChar(const QChar ch)
        {
            if (!ch.isPrint())
            {
                return false;
            }
            switch (ch.category())
            {
            case QChar::Separator_Line:
            case QChar::Separator_Paragraph:
            case QChar::Other_Format:
                return false;
            default:
                return true;
            }
        }

        // decodeAnsiChunk：按文件头注释实现——只有可见 ASCII（0x20..0x7E）原样显示，
        // 其余字节（含 0x80..0xFF 的 Latin-1 扩展区）统一显示点号；不可读字节显示占位符。
        // D11：旧代码用 QChar::isPrint() 判断，等价于按完整 Latin-1 解码，0xFF 会画出真实
        // 字符 'ÿ'、GBK 字节序列会被硬解成几个无关的 Latin-1 字符，跟文件头注释写的
        // "只认可见 ASCII"不符，且在中文系统上容易被误读成按 ACP/GBK 解码；改用
        // hexcanvas_format::IsPrintableAscii 把范围钉死在 0x20..0x7E。
        QString decodeAnsiChunk(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& validMask)
        {
            QString line;
            line.reserve(static_cast<qsizetype>(bytes.size()));
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                if (validMask[i] == 0)
                {
                    line += kUnreadableGlyph;
                    continue;
                }
                line += hexcanvas_format::IsPrintableAscii(bytes[i]) ? QChar(static_cast<char16_t>(bytes[i])) : QChar(QLatin1Char('.'));
            }
            return line;
        }

        // decodeUtf16LeChunk：按小端 UTF-16 码元两两解码；任一字节不可读则该码元整体用占位符，
        // 奇数尾字节单独处理（可读则点号，不可读则占位符），与旧 rebuildText 的 UTF-16 分支同惯例。
        // oddLeadingByte：N7（第二轮审核）——见 WorkbenchTextView.h 文件头"三种编码"的说明；
        // 为真时第一个字节是上一块被拆开的码元后半，按奇数尾字节同惯例单独处理一次，
        // 再从下标 1 开始正常两两配对，避免错位往后面所有块传染。
        QString decodeUtf16LeChunk(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& validMask, const bool oddLeadingByte)
        {
            QString line;
            std::size_t i = 0;
            if (oddLeadingByte && !bytes.empty())
            {
                line += validMask[0] == 0 ? kUnreadableGlyph : QChar(QLatin1Char('.'));
                i = 1;
            }
            for (; i + 1 < bytes.size(); i += 2)
            {
                if (validMask[i] == 0 || validMask[i + 1] == 0)
                {
                    line += kUnreadableGlyph;
                    continue;
                }
                const ushort value = static_cast<ushort>(bytes[i]) | (static_cast<ushort>(bytes[i + 1]) << 8);
                const QChar ch(value);
                line += isDisplayableChar(ch) ? ch : QChar(QLatin1Char('.'));
            }
            if (i < bytes.size())
            {
                line += validMask[i] == 0 ? kUnreadableGlyph : QChar(QLatin1Char('.'));
            }
            return line;
        }

        // decodeUtf8Chunk：手写的小型状态机，逐字符按 validMask 判断有效性（见头文件"三种编码"）。
        // D11：区分两类失败——"这个位置压根没读到数据"用 kUnreadableGlyph；"数据确实读到了，
        // 只是取值不构成合法 UTF-8"用 kInvalidUtf8SequenceGlyph；序列被这段字节的末尾截断
        // （缺续体，不是续体非法）也归为"没读到"，因为真正的原因是窗口边界，不是编码错误。
        // 非 BMP 码位（4 字节序列）是已知简化，显示为点号，不还原真实字符。
        QString decodeUtf8Chunk(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& validMask)
        {
            QString line;
            std::size_t i = 0;
            while (i < bytes.size())
            {
                if (validMask[i] == 0)
                {
                    line += kUnreadableGlyph;
                    ++i;
                    continue;
                }
                const std::uint8_t lead = bytes[i];
                int extra = 0;
                std::uint32_t codePoint = 0;
                if ((lead & 0x80U) == 0U)
                {
                    codePoint = lead;
                    extra = 0;
                }
                else if ((lead & 0xE0U) == 0xC0U)
                {
                    codePoint = lead & 0x1FU;
                    extra = 1;
                }
                else if ((lead & 0xF0U) == 0xE0U)
                {
                    codePoint = lead & 0x0FU;
                    extra = 2;
                }
                else if ((lead & 0xF8U) == 0xF0U)
                {
                    codePoint = lead & 0x07U;
                    extra = 3;
                }
                else
                {
                    // 既不是单字节也不是合法的多字节引导字节——这个字节本身已经确认可读
                    // （上面刚判过 validMask），只是取值不合法：可读但非法，不是没读到。
                    line += QChar(kInvalidUtf8SequenceGlyph);
                    ++i;
                    continue;
                }
                const bool truncatedByChunkEnd = i + static_cast<std::size_t>(extra) >= bytes.size();
                bool sequenceOk = !truncatedByChunkEnd;
                bool continuationUnreadable = false;
                for (int k = 1; sequenceOk && k <= extra; ++k)
                {
                    const std::size_t index = i + static_cast<std::size_t>(k);
                    if (validMask[index] == 0)
                    {
                        continuationUnreadable = true;
                        sequenceOk = false;
                        break;
                    }
                    if ((bytes[index] & 0xC0U) != 0x80U)
                    {
                        sequenceOk = false;
                        break;
                    }
                    codePoint = (codePoint << 6U) | (bytes[index] & 0x3FU);
                }
                if (!sequenceOk)
                {
                    // 续体缺失（被这段字节的末尾截断）或续体字节本身没读到：都是数据缺口，
                    // 不是编码错误，归为"不可读"；续体字节都在、都读到了，只是形态不对
                    // （不是 10xxxxxx）：这才是"可读但非法 UTF-8"。
                    line += (truncatedByChunkEnd || continuationUnreadable) ? kUnreadableGlyph : QChar(kInvalidUtf8SequenceGlyph);
                    ++i;
                    continue;
                }
                // 续体形态正确不等于合法 UTF-8：必须使用最短编码，且结果必须是
                // Unicode scalar（排除代理项和超过 U+10FFFF 的值）。否则 C1 A1
                // 会被误显示成普通 a，让非法字节伪装成正常文本。
                const std::uint32_t minimumCodePoint = extra == 1 ? 0x80U
                    : extra == 2 ? 0x800U : extra == 3 ? 0x10000U : 0U;
                if (codePoint < minimumCodePoint || codePoint > 0x10FFFFU
                    || (codePoint >= 0xD800U && codePoint <= 0xDFFFU))
                {
                    line += QChar(kInvalidUtf8SequenceGlyph);
                    i += static_cast<std::size_t>(1 + extra);
                    continue;
                }
                if (codePoint <= 0xFFFFU)
                {
                    const QChar ch(static_cast<ushort>(codePoint));
                    line += isDisplayableChar(ch) ? ch : QChar(QLatin1Char('.'));
                }
                else
                {
                    // 非 BMP：已知简化，不合成代理对，只标记"有一个字符但不显示其内容"。
                    line += QChar(QLatin1Char('.'));
                }
                i += static_cast<std::size_t>(1 + extra);
            }
            return line;
        }
    }

    // DecodeTextChunkForTest：按编码分派到上面三个纯函数。oddLeadingByte 只影响 UTF-16LE
    // 分支（见头文件说明，N7），ANSI/UTF-8 两条路径没有"码元边界"这个维度，忽略该参数。
    QString DecodeTextChunkForTest(
        const std::vector<std::uint8_t>& bytes,
        const std::vector<std::uint8_t>& validMask,
        const WorkbenchTextView::Encoding encoding,
        const bool oddLeadingByte)
    {
        if (bytes.size() != validMask.size())
        {
            return QString();
        }
        switch (encoding)
        {
        case WorkbenchTextView::Encoding::Utf8: return decodeUtf8Chunk(bytes, validMask);
        case WorkbenchTextView::Encoding::Utf16LE: return decodeUtf16LeChunk(bytes, validMask, oddLeadingByte);
        case WorkbenchTextView::Encoding::Ansi:
        default: return decodeAnsiChunk(bytes, validMask);
        }
    }

    WorkbenchTextView::WorkbenchTextView(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        auto* topBar = new QHBoxLayout;
        topBar->setContentsMargins(4, 4, 4, 0);
        auto* encodingLabel = new QLabel(QStringLiteral("编码："), this);
        topBar->addWidget(encodingLabel);
        m_encodingCombo = new QComboBox(this);
        m_encodingCombo->setObjectName(QStringLiteral("ksMemwbTextEncodingCombo"));
        m_encodingCombo->addItem(QStringLiteral("ANSI"));
        m_encodingCombo->addItem(QStringLiteral("UTF-8"));
        m_encodingCombo->addItem(QStringLiteral("UTF-16 LE"));
        m_encodingCombo->setToolTip(QStringLiteral("按选定编码把当前窗口的字节解码为文本；目标内容原样显示，不翻译"));
        connect(m_encodingCombo, &QComboBox::currentIndexChanged, this, [this](const int index) {
            setEncoding(static_cast<Encoding>(index));
        });
        topBar->addWidget(m_encodingCombo);
        topBar->addStretch(1);
        layout->addLayout(topBar);

        m_editor = new CodeEditorWidget(this);
        m_editor->setObjectName(QStringLiteral("ksMemwbTextEditor"));
        m_editor->setReadOnly(true);
        // 本页只展示解码文本，没有可解析的结构（JSON/属性树等），关闭结构视图切换入口。
        m_editor->setStructuredReportViewEnabled(false);
        layout->addWidget(m_editor, 1);

        m_status = new QLabel(this);
        m_status->setObjectName(QStringLiteral("ksMemwbTextStatus"));
        m_status->setText(QStringLiteral("尚未定位；跟随十六进制页的当前窗口。"));
        layout->addWidget(m_status);
    }

    void WorkbenchTextView::setBytesProvider(IWorkbenchBytesProvider* provider)
    {
        m_provider = provider;
        rebuildText();
    }

    void WorkbenchTextView::setWindow(const std::uint64_t address, const std::uint64_t length)
    {
        m_address = address;
        m_length = std::min<std::uint64_t>(length, kMaxWindowBytes);
        m_hasWindow = true;
        rebuildText();
    }

    void WorkbenchTextView::setBytesPerRow(const int bytesPerRow)
    {
        if (bytesPerRow < 1 || bytesPerRow == m_bytesPerRow)
        {
            return;
        }
        m_bytesPerRow = bytesPerRow;
        rebuildText();
    }

    int WorkbenchTextView::bytesPerRow() const
    {
        return m_bytesPerRow;
    }

    void WorkbenchTextView::setEncoding(const Encoding encoding)
    {
        if (encoding == m_encoding)
        {
            return;
        }
        m_encoding = encoding;
        if (m_encodingCombo->currentIndex() != static_cast<int>(encoding))
        {
            const QSignalBlocker blocker(m_encodingCombo);
            m_encodingCombo->setCurrentIndex(static_cast<int>(encoding));
        }
        rebuildText();
    }

    WorkbenchTextView::Encoding WorkbenchTextView::encoding() const
    {
        return m_encoding;
    }

    void WorkbenchTextView::refreshView()
    {
        rebuildText();
    }

    CodeEditorWidget* WorkbenchTextView::editor() const
    {
        return m_editor;
    }

    // minimumSizeHint：见头文件声明处的注释——故意返回一个很小的固定值，不让
    // m_editor 的尺寸偏好向上传播成宿主的硬性下限（与 WorkbenchHexPane::
    // minimumSizeHint 同一处理方式）。
    QSize WorkbenchTextView::minimumSizeHint() const
    {
        return QSize(1, 1);
    }

    // rebuildText：拉一次窗口，按 bytesPerRow 切块独立解码，拼成多行文本写回编辑器。
    void WorkbenchTextView::rebuildText()
    {
        if (m_provider == nullptr || !m_hasWindow)
        {
            m_editor->setRawText(QString());
            m_status->setText(m_provider == nullptr
                ? QStringLiteral("尚未接入数据源。")
                : QStringLiteral("尚未定位；跟随十六进制页的当前窗口。"));
            return;
        }
        const WorkbenchByteWindow window = m_provider->FetchWindow(m_address, m_length);
        if (!window.ok || window.bytes.empty())
        {
            m_editor->setRawText(QString());
            // D4：数字部分单独大写再 .arg() 进模板，不对整段模板调用 toUpper()。
            m_status->setText(QStringLiteral("0x%1 超出已读取窗口。").arg(formatHexDigitsUpper(m_address, 16)));
            return;
        }
        // 可疑点 4：防御性 min 夹取，避免一个实现有误的 provider 让 bytes 与 validMask
        // 长度不一致时越界读。
        const std::size_t effectiveLength = std::min(window.bytes.size(), window.validMask.size());
        QString text;
        text.reserve(static_cast<qsizetype>(effectiveLength));
        // 可疑点 6：第一行按"绝对地址对齐到 bytesPerRow 的倍数"裁短，让后续每一行的换行
        // 位置落在与十六进制页同一套绝对地址网格上；m_address 本身恰好对齐时第一行就是
        // 满宽的一整行，不改变既有行为（夹具里现有的用例全部是整齐对齐的窗口起点）。
        const auto alignment = static_cast<std::uint64_t>(m_bytesPerRow);
        const std::uint64_t leadInBytes = m_address % alignment;
        std::size_t firstChunkLength = static_cast<std::size_t>(leadInBytes == 0 ? alignment : alignment - leadInBytes);
        firstChunkLength = std::min(firstChunkLength, effectiveLength);

        using Offset = std::vector<std::uint8_t>::difference_type;
        bool firstRow = true;
        std::size_t offset = 0;
        while (offset < effectiveLength)
        {
            const std::size_t chunkLength = firstRow
                ? firstChunkLength
                : std::min(static_cast<std::size_t>(m_bytesPerRow), effectiveLength - offset);
            const std::vector<std::uint8_t> chunkBytes(window.bytes.begin() + static_cast<Offset>(offset),
                window.bytes.begin() + static_cast<Offset>(offset + chunkLength));
            const std::vector<std::uint8_t> chunkMask(window.validMask.begin() + static_cast<Offset>(offset),
                window.validMask.begin() + static_cast<Offset>(offset + chunkLength));
            if (offset != 0)
            {
                text += QLatin1Char('\n');
            }
            // N7：这一块相对窗口起点的字节偏移（offset）若是奇数，说明它在真正的 UTF-16
            // 码元网格上起点是奇数——窗口起点本身（offset==0）永远被当作码元网格的基准，
            // 不属于这种情况；只有因为按 bytesPerRow 对齐裁出的首行长度是奇数时，第二行及
            // 之后才会落在奇数偏移上（见头文件"三种编码"的 N7 说明）。
            text += DecodeTextChunkForTest(chunkBytes, chunkMask, m_encoding, (offset % 2) != 0);
            offset += chunkLength;
            firstRow = false;
        }
        // setRawText：目标内容原样显示，绝不经过语言包翻译（文件头说明）。
        m_editor->setRawText(text);
        m_status->setText(QStringLiteral("0x%1 起 %2 字节，按 %3 行宽解码（%4）。")
            .arg(formatHexDigitsUpper(m_address, 16)).arg(window.bytes.size()).arg(m_bytesPerRow)
            .arg(m_encodingCombo->currentText()));
    }
}
