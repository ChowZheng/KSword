// HexInspectorPanel.Rows.cpp
// 作用：数据解释器面板"取字节、解释、变成三列文字"的部分。
// - collectWindow：从画布取插入点起最多 16 个"所见值"字节，遇到没有值的字节就停（绝不补 0）；
// - buildRows：把连续字节交给 ksword::memwb::DecodeAll，再把每一行的结果整理成 类型名 | 值 | 十六进制。
// 本文件没有任何目标内存读写，也不重写解释规则。

#include "HexInspectorPanel.h"

#include <QStringList>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace ks::ui
{
    namespace
    {
        // RowKind：一行属于哪一族，决定"值/十六进制"两列怎么由 DecodeAll 的结果整理。
        enum class RowKind : int
        {
            Integer = 0,    // i8..u64：text 形如 "-1 (0xFF)"
            Float,          // f32/f64
            Pointer,        // ptr：text 形如 "0x... (module+0x..)"
            Time,           // filetime/time_t32/time_t64
            Guid,           // guid
            Text            // ascii/utf16
        };

        // RowSpec：一行的静态描述。
        struct RowSpec
        {
            const char* key;        // DecodeAll 的 label，也是 EncodeValue 的类型名
            const char* name;       // 类型列显示名（ptr 行的名字随指针宽度变化，这里留空由代码补）
            int widthBytes;         // 固定占用字节数；0 表示变长或随指针宽度
            RowKind kind;           // 所属族
            bool editable;          // 是否支持行内编辑
        };

        // kSpecs：与 DecodeAll 的固定行顺序一一对应（查找时按 key 匹配，不依赖下标）。
        constexpr RowSpec kSpecs[] = {
            { "i8", "int8", 1, RowKind::Integer, true },
            { "u8", "uint8", 1, RowKind::Integer, true },
            { "i16", "int16", 2, RowKind::Integer, true },
            { "u16", "uint16", 2, RowKind::Integer, true },
            { "i32", "int32", 4, RowKind::Integer, true },
            { "u32", "uint32", 4, RowKind::Integer, true },
            { "i64", "int64", 8, RowKind::Integer, true },
            { "u64", "uint64", 8, RowKind::Integer, true },
            { "f32", "float", 4, RowKind::Float, true },
            { "f64", "double", 8, RowKind::Float, true },
            { "ptr", "", 0, RowKind::Pointer, true },
            { "filetime", "FILETIME", 8, RowKind::Time, false },
            { "time_t32", "time_t32", 4, RowKind::Time, false },
            { "time_t64", "time_t64", 8, RowKind::Time, false },
            { "guid", "GUID", 16, RowKind::Guid, false },
            { "ascii", "ASCII", 0, RowKind::Text, false },
            { "utf16", "UTF-16", 0, RowKind::Text, false },
        };

        // FindSpec：按 DecodeAll 的 label 找静态描述；找不到返回空指针（调用方跳过该行）。
        const RowSpec* FindSpec(const std::string& label)
        {
            for (const RowSpec& spec : kSpecs)
            {
                if (label == spec.key)
                {
                    return &spec;
                }
            }
            return nullptr;
        }

        // FromUtf8：DecodeAll 输出的 UTF-8 文本转 QString。
        QString FromUtf8(const std::string& text)
        {
            return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        }

        // PatternOf：按字节序把 width 个字节拼成无符号位模式。
        // 传入：字节起点（调用方保证至少 width 字节）、宽度（1..8）、字节序；传出：位模式。
        std::uint64_t PatternOf(const std::uint8_t* bytes, std::size_t width, ksword::memwb::ByteOrder order)
        {
            std::uint64_t value = 0;
            for (std::size_t index = 0; index < width; ++index)
            {
                // 小端时最高有效字节在末尾，大端在开头。
                const std::size_t source = (order == ksword::memwb::ByteOrder::Little) ? (width - 1 - index) : index;
                value = (value << 8) | bytes[source];
            }
            return value;
        }

        // HexPattern：位模式格式化成 "0x" + 补零到 2*width 位的大写十六进制。
        QString HexPattern(std::uint64_t value, std::size_t width)
        {
            return QStringLiteral("0x")
                + QString::number(value, 16).toUpper().rightJustified(static_cast<int>(width * 2), QLatin1Char('0'));
        }

        // HexOfBytes：字节转 "48 65 6C" 形式（内存顺序，大写，空格分隔）。
        // 传入：起点、字节数、最多显示多少个；超过时末尾加省略号。
        QString HexOfBytes(const std::uint8_t* bytes, std::size_t count, std::size_t maxShown)
        {
            QStringList parts;
            const std::size_t shown = std::min(count, maxShown);
            for (std::size_t index = 0; index < shown; ++index)
            {
                parts.push_back(QStringLiteral("%1").arg(static_cast<int>(bytes[index]), 2, 16, QLatin1Char('0')).toUpper());
            }
            QString text = parts.join(QLatin1Char(' '));
            if (count > shown)
            {
                text += QStringLiteral(" …");
            }
            return text;
        }
    }

    // 取字节窗口：来自画布"所见值"，连续取到第一个没有值的字节为止。
    HexInspectorPanel::WindowSnapshot HexInspectorPanel::collectWindow() const
    {
        WindowSnapshot snapshot;
        HexCanvas* canvasPointer = m_canvas.data();
        if (canvasPointer == nullptr)
        {
            return snapshot;
        }
        // 身份也属于快照：换目标后即便地址和字节相同，旧输入仍必须失效。
        snapshot.sourceRevision = canvasPointer->sourceRevision();
        snapshot.overlay = canvasPointer->overlay();
        if (snapshot.overlay != nullptr)
        {
            snapshot.identityKey = snapshot.overlay->IdentityKey();
        }
        const std::optional<HexCanvas::AddressRange> selection = canvasPointer->selectedRange();
        if (!selection.has_value())
        {
            return snapshot;
        }

        // 基本信息：插入点、选区大小（整个 64 位空间的全选会让"个数"回绕，夹成最大值）、是否可编辑。
        snapshot.hasData = true;
        snapshot.address = canvasPointer->caretAddress();
        const std::uint64_t span = selection->last - selection->first;
        snapshot.selectionBytes = (span == std::numeric_limits<std::uint64_t>::max()) ? span : span + 1ULL;
        snapshot.canEdit = canvasPointer->isEditable() && canvasPointer->overlay() != nullptr;
        snapshot.gap = GapReason::None;

        // 逐字节取"所见值"，没有值就记下断在哪个原因上并停止；地址加法先判溢出。
        snapshot.bytes.reserve(static_cast<std::size_t>(kWindowBytes));
        for (int offset = 0; offset < kWindowBytes; ++offset)
        {
            const std::uint64_t step = static_cast<std::uint64_t>(offset);
            if (snapshot.address > std::numeric_limits<std::uint64_t>::max() - step)
            {
                snapshot.gap = GapReason::EndOfSpace;
                break;
            }
            const HexCanvas::CellState cell = canvasPointer->cellStateAt(snapshot.address + step);
            if (cell.hasValue)
            {
                snapshot.bytes.push_back(cell.value);
                continue;
            }
            if (!cell.inSpace)
            {
                snapshot.gap = GapReason::EndOfSpace;
            }
            else if (cell.byteState == HexCanvas::ByteState::Unreadable)
            {
                snapshot.gap = GapReason::Unreadable;
            }
            else
            {
                snapshot.gap = GapReason::NotLoaded;
            }
            break;
        }
        return snapshot;
    }

    // 不可用行的悬停提示：说明需要几个字节、实际连续可用几个、断在什么原因上。
    QString HexInspectorPanel::unavailableTip(const QString& typeName, std::size_t needBytes) const
    {
        if (!m_window.hasData)
        {
            return typeName + QLatin1Char('\n') + QStringLiteral("没有数据：画布还没有载入地址空间");
        }

        // 连续可用的字节已经够数，说明是变长行（字符串）在起点处没有可显示的字符。
        const std::size_t have = m_window.bytes.size();
        if (have >= needBytes)
        {
            return typeName + QLatin1Char('\n') + QStringLiteral("插入点处没有可显示的字符（遇到结束符或不可打印的字节）");
        }

        // 字节不够：按断点原因给出后半句。
        QString reason;
        switch (m_window.gap)
        {
        case GapReason::NotLoaded:
            reason = QStringLiteral("后面的字节尚未加载");
            break;
        case GapReason::Unreadable:
            reason = QStringLiteral("后面的字节不可读");
            break;
        case GapReason::EndOfSpace:
            reason = QStringLiteral("已到地址空间末尾");
            break;
        case GapReason::None:
        case GapReason::NoData:
            break;
        }
        return typeName + QLatin1Char('\n')
            + QStringLiteral("需要 %1 个字节，从插入点起连续可用的只有 %2 个（%3）")
                .arg(needBytes)
                .arg(have)
                .arg(reason);
    }

    // 把 DecodeAll 的结果整理成行。
    std::vector<HexInspectorRowData> HexInspectorPanel::buildRows() const
    {
        // 解释：只用连续可用的前缀，字节不足的行由 DecodeAll 标记 available=false。
        const std::uint8_t* bytes = m_window.bytes.empty() ? nullptr : m_window.bytes.data();
        const std::size_t count = m_window.bytes.size();
        const std::vector<ksword::memwb::DecodedRow> decoded =
            ksword::memwb::DecodeAll(bytes, count, m_order, m_pointerWidth, m_namer);

        std::vector<HexInspectorRowData> rows;
        rows.reserve(decoded.size());
        for (const ksword::memwb::DecodedRow& source : decoded)
        {
            const RowSpec* spec = FindSpec(source.label);
            if (spec == nullptr)
            {
                continue;
            }

            // 行的基本信息：键、显示名（ptr 行带上宽度）、是否支持编辑。
            HexInspectorRowData row;
            row.typeKey = QString::fromLatin1(spec->key);
            if (spec->kind == RowKind::Pointer)
            {
                row.typeName = (m_pointerWidth == 4U) ? QStringLiteral("ptr32") : QStringLiteral("ptr64");
            }
            else
            {
                row.typeName = QString::fromLatin1(spec->name);
            }
            row.editable = spec->editable;
            row.editEnabled = spec->editable && m_window.canEdit;
            row.available = source.available;
            row.valid = source.valid;

            // 该行至少需要的字节数：固定宽度、指针宽度、字符串的最小字符。
            std::size_t needBytes = static_cast<std::size_t>(spec->widthBytes);
            if (spec->kind == RowKind::Pointer)
            {
                needBytes = m_pointerWidth;
            }
            else if (spec->kind == RowKind::Text)
            {
                needBytes = (std::string_view(spec->key) == "utf16") ? 2U : 1U;
            }

            // 不可用：整行置灰，没有任何可复制内容，提示说明原因。
            // 值列的措辞要分两种：字节不够（真的"不可用"）与字节够了但起点处没有字符（字符串行，"没有内容"）。
            if (!source.available)
            {
                row.valueText = (count >= needBytes) ? QStringLiteral("无可显示的字符") : QStringLiteral("不可用");
                row.hexText = QStringLiteral("—");
                row.toolTip = unavailableTip(row.typeName, needBytes);
                rows.push_back(std::move(row));
                continue;
            }

            // 可用：先算出值列/十六进制列/复制内容与"该行占用的原始字节数"。
            const QString text = FromUtf8(source.text);
            row.valueCopy = FromUtf8(source.copyText);
            std::size_t spanBytes = needBytes;
            bool truncated = false;
            switch (spec->kind)
            {
            case RowKind::Integer:
            {
                // "305419896 (0x12345678)" 拆成十进制与位模式两列：左括号之前是十进制，括号里是位模式；
                // 找不到括号就整串放值列、自己算位模式。
                const qsizetype open = text.indexOf(QLatin1Char('('));
                if (open > 1 && text.endsWith(QLatin1Char(')')))
                {
                    row.valueText = text.left(open).trimmed();
                    row.hexText = text.mid(open + 1, text.size() - open - 2);
                }
                else
                {
                    row.valueText = text;
                    row.hexText = HexPattern(PatternOf(bytes, spanBytes, m_order), spanBytes);
                }
                row.hexCopy = row.hexText;
                break;
            }
            case RowKind::Float:
            case RowKind::Time:
            {
                // 浮点与时间：值列用 DecodeAll 的文本（时间超范围换成中文说明），十六进制列是位模式。
                row.valueText = (spec->kind == RowKind::Time && !source.valid) ? QStringLiteral("超出范围") : text;
                row.hexText = HexPattern(PatternOf(bytes, spanBytes, m_order), spanBytes);
                row.hexCopy = row.hexText;
                break;
            }
            case RowKind::Pointer:
            {
                // "0x... (描述)"：十六进制列放地址，值列优先放描述，没有描述就与地址相同。
                // 地址部分不含括号，所以第一个左括号就是描述的开头（描述自己可以含括号）。
                const qsizetype open = text.indexOf(QLatin1Char('('));
                const QString addressPart = (open > 0) ? text.left(open).trimmed() : text;
                const QString description = (open > 0 && text.endsWith(QLatin1Char(')')))
                    ? text.mid(open + 1, text.size() - open - 2)
                    : QString();
                row.valueText = description.isEmpty() ? addressPart : description;
                row.hexText = addressPart;
                row.hexCopy = addressPart;
                break;
            }
            case RowKind::Guid:
            {
                // GUID：值列是 8-4-4-4-12 文本，十六进制列是起点处的原始字节（最多显示 8 个）。
                row.valueText = text;
                row.hexText = HexOfBytes(bytes, spanBytes, 8);
                row.hexCopy = HexOfBytes(bytes, spanBytes, spanBytes);
                break;
            }
            case RowKind::Text:
            {
                // 字符串：占用的字节数 = ASCII 的字符数 / UTF-16 的码元数乘 2；
                // 取满 16 个字节仍没遇到结束符，说明字符串可能更长，值末尾补省略号并在提示里说明。
                const bool isUtf16 = (std::string_view(spec->key) == "utf16");
                spanBytes = isUtf16 ? static_cast<std::size_t>(text.size()) * 2U : static_cast<std::size_t>(text.size());
                spanBytes = std::min(spanBytes, count);
                truncated = (count == static_cast<std::size_t>(kWindowBytes))
                    && (spanBytes + (isUtf16 ? 1U : 0U) >= count);
                row.valueText = truncated ? (text + QStringLiteral("…")) : text;
                row.hexText = HexOfBytes(bytes, spanBytes, 8);
                row.hexCopy = HexOfBytes(bytes, spanBytes, spanBytes);
                break;
            }
            }

            // 悬停提示：类型、值、十六进制、原始字节（内存顺序），再加上必要的说明与操作提示。
            QStringList lines;
            lines << row.typeName;
            lines << QStringLiteral("值：%1").arg(text);
            lines << QStringLiteral("十六进制：%1").arg(row.hexCopy);
            lines << QStringLiteral("字节（内存顺序）：%1").arg(HexOfBytes(bytes, spanBytes, spanBytes));
            if (truncated)
            {
                lines << QStringLiteral("仅解析前 16 字节，字符串可能更长");
            }
            if (!source.valid)
            {
                lines << (spec->kind == RowKind::Time
                    ? QStringLiteral("时间值超出可显示范围；复制得到的是原始数值")
                    : QStringLiteral("含有孤立的代理项，已用 U+FFFD 替换"));
            }
            if (!spec->editable)
            {
                lines << QStringLiteral("此类型是只读的；右键复制");
            }
            else if (m_window.canEdit)
            {
                lines << QStringLiteral("双击编辑；右键复制");
            }
            else
            {
                lines << QStringLiteral("当前为只读视图，不能编辑；右键复制");
            }
            row.toolTip = lines.join(QLatin1Char('\n'));
            rows.push_back(std::move(row));
        }
        return rows;
    }
}
