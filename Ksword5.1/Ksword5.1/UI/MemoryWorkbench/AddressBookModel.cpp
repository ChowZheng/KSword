// AddressBookModel.cpp
// 作用：AddressBookModel.h 声明的实现——表格数据、列格式化、kind 过滤、类型图标绘制。

#include "AddressBookModel.h"

#include "AddressBookStore.h"

#include "../../theme.h"
#include "../../Internationalization/LanguageManager.h"

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QStringList>

#include <algorithm>
#include <set>

namespace ks::ui
{
    namespace
    {
        // kKindIconSize：类型图标的像素边长；表格行高通常在 22-26px 之间，16px 留出上下留白。
        constexpr int kKindIconSize = 16;

        // FormatHex：把 64 位数格式化成 "0x" + 最短小写十六进制（0 显示为 "0x0"）。
        // 与 MemoryAddressBook.Serialize.cpp 里的 FormatHexAddress 规则一致，这里用 Qt 字符串
        // 重新实现一遍（避免 UI 层反向依赖 Core 的匿名命名空间内部函数）。
        QString FormatHex(const std::uint64_t value)
        {
            return QStringLiteral("0x%1").arg(value, 0, 16);
        }

        QString FormatPointerOffsets(const ksword::memwb::PointerBookmarkDefinition& definition)
        {
            QStringList offsets;
            for (const std::int64_t offset : definition.offsets)
            {
                const std::uint64_t magnitude = offset < 0
                    ? static_cast<std::uint64_t>(-(offset + 1)) + 1U
                    : static_cast<std::uint64_t>(offset);
                offsets << (offset < 0 ? QStringLiteral("-") : QString()) + FormatHex(magnitude);
            }
            return offsets.join(QStringLiteral(", "));
        }

        QString FormatPointerChain(const ksword::memwb::AddressEntry& entry)
        {
            return ks::i18n::sourceText(QStringLiteral("指针链：%1+%2 → %3"))
                .arg(QString::fromUtf8(entry.moduleName.c_str()), FormatHex(entry.rva),
                    FormatPointerOffsets(*entry.pointerChain));
        }

        QString PointerChainToolTip(const ksword::memwb::AddressEntry& entry)
        {
            const auto& definition = *entry.pointerChain;
            return ks::i18n::sourceText(QStringLiteral(
                "基址模块：%1\n根指针偏移：%2\n逐级偏移（从根到目标）：%3\n绑定程序：%4\n指针宽度：%5 位\n仅按需解析，不自动刷新。"))
                .arg(QString::fromUtf8(definition.modulePath.c_str()), FormatHex(entry.rva),
                    FormatPointerOffsets(definition), QString::fromUtf8(definition.processPath.c_str()))
                .arg(definition.pointerSize * 8U);
        }

        // A chain row shows its definition, never a root address posing as the target.
        QString FormatAddressColumn(const ksword::memwb::AddressEntry& entry)
        {
            if (entry.pointerChain.has_value())
            {
                return FormatPointerChain(entry);
            }
            if (!entry.moduleName.empty())
            {
                return QStringLiteral("%1+%2")
                    .arg(QString::fromUtf8(entry.moduleName.c_str()))
                    .arg(FormatHex(entry.rva));
            }
            return FormatHex(entry.absoluteAddress);
        }

        // FormatModuleOffsetColumn："模块+RVA"列：只有模块条目才有值，绝对地址条目显示占位
        // "—"（em dash），用于在 B 组一眼看出"这条是不是绑定着某个模块"。
        QString FormatModuleOffsetColumn(const ksword::memwb::AddressEntry& entry)
        {
            if (entry.pointerChain.has_value())
            {
                return FormatPointerChain(entry);
            }
            if (entry.moduleName.empty())
            {
                return QStringLiteral("—");
            }
            return QStringLiteral("%1 + %2")
                .arg(QString::fromUtf8(entry.moduleName.c_str()))
                .arg(FormatHex(entry.rva));
        }

        // KindDisplayName：EntryKind 的中文源文本，同时用作类型图标列的悬停提示。
        // 返回的是源文本（未经语言包翻译）——调用方自己决定要不要经 ks::i18n::sourceText
        // 转换；AddressBookPanel.Menu.cpp 的"加入书签/监视"等动作文案与本函数无关，不复用它。
        QString KindDisplayName(const ksword::memwb::EntryKind kind)
        {
            switch (kind)
            {
            case ksword::memwb::EntryKind::Search: return QStringLiteral("搜索结果");
            case ksword::memwb::EntryKind::Bookmark: return QStringLiteral("书签");
            case ksword::memwb::EntryKind::Watch: return QStringLiteral("监视");
            default: return QStringLiteral("未知");
            }
        }

        // FormatAddressSortKey："地址"列的排序键（修复 C15）：按字符串比较也能得到正确的
        // 数值序——绝对地址用零填充 16 位十六进制；模块地址先按模块名、再按 RVA（同样零填充）
        // 分组排序。两类地址没有共同的数值意义（模块基址运行期才解析），用前缀把它们分成
        // "0:"（绝对地址组）与"1:"（模块地址组）两段，组间顺序是一个确定性的展示选择，
        // 不代表真实地址高低。
        QString FormatAddressSortKey(const ksword::memwb::AddressEntry& entry)
        {
            if (entry.pointerChain.has_value())
            {
                return QStringLiteral("2:%1:%2:%3")
                    .arg(QString::fromUtf8(entry.moduleName.c_str()))
                    .arg(entry.rva, 16, 16, QChar(u'0'))
                    .arg(FormatPointerOffsets(*entry.pointerChain));
            }
            if (!entry.moduleName.empty())
            {
                return QStringLiteral("1:%1:%2")
                    .arg(QString::fromUtf8(entry.moduleName.c_str()))
                    .arg(entry.rva, 16, 16, QChar(u'0'));
            }
            return QStringLiteral("0:%1").arg(entry.absoluteAddress, 16, 16, QChar(u'0'));
        }

        // BuildKindIcon：按 kind 现场画一个小图标，不依赖 Ksword5.qrc（省一次新增别名）。
        // 颜色在每次调用时现取 KswordTheme 的静态颜色访问器，保证深浅主题切换后下一次绘制
        // 就是新主题的颜色——这是 HexViewWidgets 一族"全部自绘"同样的做法（见
        // HexViewWidgets.h 文件头的说明），本函数不做任何缓存。
        QIcon BuildKindIcon(const ksword::memwb::EntryKind kind)
        {
            QPixmap pixmap(kKindIconSize, kKindIconSize);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing, true);

            switch (kind)
            {
            case ksword::memwb::EntryKind::Search:
            {
                // 放大镜：一个圆 + 右下角一条短柄。
                const QColor strokeColor = KswordTheme::TextSecondaryColor();
                painter.setPen(QPen(strokeColor, 1.6));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(QRectF(2.0, 2.0, 8.0, 8.0));
                painter.drawLine(QPointF(9.5, 9.5), QPointF(13.5, 13.5));
                break;
            }
            case ksword::memwb::EntryKind::Bookmark:
            {
                // 书签丝带：矩形 + 底边 V 形缺口，实心填充主题强调色。
                const QColor fillColor = KswordTheme::PrimaryBlueColor;
                QPolygonF ribbon;
                ribbon << QPointF(4.0, 1.5) << QPointF(12.0, 1.5) << QPointF(12.0, 14.5)
                       << QPointF(8.0, 11.0) << QPointF(4.0, 14.5);
                painter.setPen(Qt::NoPen);
                painter.setBrush(fillColor);
                painter.drawPolygon(ribbon);
                break;
            }
            case ksword::memwb::EntryKind::Watch:
            default:
            {
                // 眼睛：杏仁形轮廓 + 实心瞳孔，用信息色，呼应"持续观察"的语义。
                const QColor strokeColor = KswordTheme::InfoColor();
                painter.setPen(QPen(strokeColor, 1.4));
                painter.setBrush(Qt::NoBrush);
                QPainterPath eye;
                eye.moveTo(1.5, 8.0);
                eye.quadTo(8.0, 1.5, 14.5, 8.0);
                eye.quadTo(8.0, 14.5, 1.5, 8.0);
                painter.drawPath(eye);
                painter.setPen(Qt::NoPen);
                painter.setBrush(strokeColor);
                painter.drawEllipse(QRectF(6.0, 6.0, 4.0, 4.0));
                break;
            }
            }
            painter.end();
            return QIcon(pixmap);
        }
    }

    QString AddressBookModel::ValueTypeDisplayName(const ksword::memwb::ValueType valueType)
    {
        switch (valueType)
        {
        case ksword::memwb::ValueType::Hex8: return QStringLiteral("十六进制(8字节)");
        case ksword::memwb::ValueType::U8: return QStringLiteral("无符号8位");
        case ksword::memwb::ValueType::U16: return QStringLiteral("无符号16位");
        case ksword::memwb::ValueType::U32: return QStringLiteral("无符号32位");
        case ksword::memwb::ValueType::U64: return QStringLiteral("无符号64位");
        case ksword::memwb::ValueType::I8: return QStringLiteral("有符号8位");
        case ksword::memwb::ValueType::I16: return QStringLiteral("有符号16位");
        case ksword::memwb::ValueType::I32: return QStringLiteral("有符号32位");
        case ksword::memwb::ValueType::I64: return QStringLiteral("有符号64位");
        case ksword::memwb::ValueType::F32: return QStringLiteral("单精度浮点");
        case ksword::memwb::ValueType::F64: return QStringLiteral("双精度浮点");
        default: return QStringLiteral("未知类型");
        }
    }

    AddressBookModel::AddressBookModel(AddressBookStore* store, QObject* parent)
        : QAbstractTableModel(parent)
        , m_store(store)
    {
        if (m_store != nullptr)
        {
            connect(m_store, &AddressBookStore::entryAdded, this, &AddressBookModel::onStoreEntryAdded);
            connect(m_store, &AddressBookStore::entryRemoved, this, &AddressBookModel::onStoreEntryRemoved);
            connect(m_store, &AddressBookStore::entryChanged, this, &AddressBookModel::onStoreEntryChanged);
            connect(m_store, &AddressBookStore::reset, this, &AddressBookModel::onStoreReset);
            // 修复 D7 后半：load() 整本替换成功时单独清空值缓存（见 bookReloaded 的注释）。
            connect(m_store, &AddressBookStore::bookReloaded, this, &AddressBookModel::onStoreBookReloaded);
            // 修复 D9：Store 先于本模型销毁时主动整表重建一次（见 onStoreDestroyed 注释）。
            // m_store 是 QPointer，这个连接建立时它已经在跟踪 store 的生命周期，真正销毁时
            // Qt 会先清空各处的 QPointer 守卫、再把 destroyed() 派发给普通信号槛连接，所以
            // 本槛执行时 m_store 已经是 nullptr——rebuildRows 里的 computeCountsAndRowIds
            // 对此有专门的判空分支。
            connect(m_store, &QObject::destroyed, this, &AddressBookModel::onStoreDestroyed);
        }
        rebuildRows();
    }

    int AddressBookModel::rowCount(const QModelIndex& parent) const
    {
        if (parent.isValid())
        {
            return 0;
        }
        return static_cast<int>(m_rowIds.size());
    }

    int AddressBookModel::columnCount(const QModelIndex& parent) const
    {
        if (parent.isValid())
        {
            return 0;
        }
        return static_cast<int>(ColumnCount);
    }

    QVariant AddressBookModel::data(const QModelIndex& index, const int role) const
    {
        if (!index.isValid())
        {
            return QVariant();
        }
        const std::optional<ksword::memwb::AddressEntry> entryOpt = entryForRow(index.row());
        if (!entryOpt.has_value())
        {
            return QVariant();
        }
        const ksword::memwb::AddressEntry& entry = *entryOpt;

        // 与列无关的角色先处理：id/kind 角色对任意列都返回同一个值，方便 Panel 不管点中
        // 哪一列都能直接换回 id。
        if (role == IdRole)
        {
            return QVariant::fromValue<quint64>(entry.id);
        }
        if (role == KindRole)
        {
            return static_cast<int>(entry.kind);
        }
        if (role == ValueTypeRole)
        {
            return static_cast<int>(entry.valueType);
        }
        if (role == SortRole)
        {
            // 修复 C15：排序键按列单独给，不能直接复用 DisplayRole 的格式化文本
            // （地址列尤其明显：十六进制字符串的字典序与数值大小完全不是一回事）。
            switch (static_cast<Column>(index.column()))
            {
            case ColumnKindIcon: return static_cast<int>(entry.kind);
            case ColumnAddress: return FormatAddressSortKey(entry);
            case ColumnValueType: return static_cast<int>(entry.valueType);
            case ColumnValue:
            {
                if (entry.pointerChain.has_value()) return QString();
                const auto cellIt = m_valueCells.find(entry.id);
                return cellIt != m_valueCells.end() ? cellIt->second.text : QString();
            }
            case ColumnNote: return QString::fromUtf8(entry.note.c_str());
            case ColumnModuleOffset: return FormatModuleOffsetColumn(entry);
            case ColumnTarget:
            {
                const QString key = QString::fromUtf8(entry.targetKey.c_str());
                const auto nameIt = m_targetNames.find(key);
                return nameIt != m_targetNames.end() ? nameIt->second : key;
            }
            default: return QVariant();
            }
        }

        switch (static_cast<Column>(index.column()))
        {
        case ColumnKindIcon:
            if (role == Qt::DecorationRole)
            {
                return BuildKindIcon(entry.kind);
            }
            if (role == Qt::ToolTipRole)
            {
                // 纯 UI 文案（"搜索结果/书签/监视"），不是用户/目标数据，经语言包翻译（C12）。
                return entry.pointerChain.has_value()
                    ? ks::i18n::sourceText(QStringLiteral("指针链书签"))
                    : ks::i18n::sourceText(KindDisplayName(entry.kind));
            }
            return QVariant();

        case ColumnAddress:
            if (role == Qt::ToolTipRole && entry.pointerChain.has_value())
            {
                return PointerChainToolTip(entry);
            }
            // 地址是目标数据（模块名+RVA 或绝对地址），不翻译（C12 的决策明确排除此列）。
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
            {
                return FormatAddressColumn(entry);
            }
            return QVariant();

        case ColumnValue:
        {
            if (entry.pointerChain.has_value())
            {
                if (role == Qt::DisplayRole)
                    return ks::i18n::sourceText(QStringLiteral("按需解析"));
                if (role == Qt::ToolTipRole)
                    return ks::i18n::sourceText(QStringLiteral("指针链仅按需解析，不自动刷新。"));
                if (role == Qt::ForegroundRole) return KswordTheme::TextSecondaryColor();
                if (role == ValueStateRole) return static_cast<int>(ValueState::NotRead);
                return QVariant();
            }
            // ValueCell：外部喂入的文本与状态；找不到（从未喂入过）时按 NotRead 显示占位。
            const auto cellIt = m_valueCells.find(entry.id);
            const ValueCell cell = cellIt != m_valueCells.end() ? cellIt->second : ValueCell{};
            if (role == Qt::DisplayRole)
            {
                if (!cell.text.isEmpty())
                {
                    // 真正读到的数据，不是 UI 文案，不翻译（C12 的决策同样排除这一类数据）。
                    return cell.text;
                }
                switch (cell.state)
                {
                case ValueState::Reading: return ks::i18n::sourceText(QStringLiteral("读取中…"));
                case ValueState::Unreadable: return ks::i18n::sourceText(QStringLiteral("不可读"));
                default: return ks::i18n::sourceText(QStringLiteral("—"));
                }
            }
            if (role == Qt::ToolTipRole)
            {
                switch (cell.state)
                {
                case ValueState::NotRead: return ks::i18n::sourceText(QStringLiteral("尚未读取"));
                case ValueState::Reading: return ks::i18n::sourceText(QStringLiteral("正在读取"));
                case ValueState::Read:
                    return ks::i18n::sourceText(QStringLiteral("已读取：%1")).arg(cell.text);
                case ValueState::Unreadable: return ks::i18n::sourceText(QStringLiteral("此地址当前不可读"));
                case ValueState::Stale:
                    return ks::i18n::sourceText(QStringLiteral("目标已变化，这是过期值，重读后更新"));
                }
                return QVariant();
            }
            if (role == Qt::ForegroundRole)
            {
                // 绘制用色：现取主题静态颜色，不缓存，深浅主题切换后下一次绘制自动跟随。
                switch (cell.state)
                {
                case ValueState::NotRead: return KswordTheme::TextSecondaryColor();
                case ValueState::Reading: return KswordTheme::InfoColor();
                case ValueState::Unreadable: return KswordTheme::ErrorColor();
                case ValueState::Stale: return KswordTheme::WarningColor();
                case ValueState::Read: return KswordTheme::TextPrimaryColor();
                }
                return QVariant();
            }
            if (role == ValueStateRole)
            {
                return static_cast<int>(cell.state);
            }
            return QVariant();
        }

        case ColumnValueType:
            // 纯 UI 文案（"无符号32位"等 11 种固定名字），经语言包翻译（C12）。
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
            {
                return ks::i18n::sourceText(ValueTypeDisplayName(entry.valueType));
            }
            return QVariant();

        case ColumnNote:
            // 备注是用户自己敲的文本，不是 UI 文案，不翻译（C12 的决策明确排除此列）。
            if (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::ToolTipRole)
            {
                return QString::fromUtf8(entry.note.c_str());
            }
            return QVariant();

        case ColumnModuleOffset:
            if (role == Qt::ToolTipRole && entry.pointerChain.has_value())
            {
                return PointerChainToolTip(entry);
            }
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
            {
                return FormatModuleOffsetColumn(entry);
            }
            return QVariant();

        case ColumnTarget:
            // 目标展示名是调用方给的数据（进程名/PID 一类），不翻译（C12 的决策明确排除）。
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
            {
                const QString key = QString::fromUtf8(entry.targetKey.c_str());
                const auto nameIt = m_targetNames.find(key);
                if (nameIt != m_targetNames.end())
                {
                    return nameIt->second;
                }
                return key.isEmpty() ? QStringLiteral("—") : key;
            }
            return QVariant();

        default:
            return QVariant();
        }
    }

    bool AddressBookModel::setData(const QModelIndex& index, const QVariant& value, const int role)
    {
        if (!index.isValid() || role != Qt::EditRole || m_store == nullptr)
        {
            return false;
        }
        const std::uint64_t id = idAt(index);
        if (id == 0)
        {
            return false;
        }

        if (index.column() == ColumnNote)
        {
            // 备注：纯地址簿记录，不涉及任何内存读写，Model 可以直接落库。
            if (!m_store->setNote(id, value.toString()))
            {
                return false;
            }
            // Store::setNote 已经会发 entryChanged 触发 onStoreEntryChanged -> dataChanged，
            // 这里不用重复发，避免同一次编辑触发两次视图刷新。
            return true;
        }
        if (index.column() == ColumnValueType)
        {
            // 值类型同样是纯记录（决定"这段字节按什么类型显示"），不涉及内存读写；本列不再
            // 通过 flags() 标记可编辑（C3：双击表格格子不再能改类型，只能走右键"值类型▸"
            // 子菜单），但菜单正是经由本函数直接调用 setData 来提交，所以这里仍要保留实现。
            // 修复 C3 的类型校验部分：value.toInt() 对一个字符串（例如用户手打的中文类型名）
            // 也会"成功"地转成 0，静默把类型改成 Hex8——这里改用 toInt(&ok) 并要求调用方
            // 传入的是真正的 int 类型（QMetaType::Int），拒绝任何字符串。
            if (value.typeId() != QMetaType::Int)
            {
                return false;
            }
            bool parsedOk = false;
            const int rawType = value.toInt(&parsedOk);
            if (!parsedOk || rawType < 0 || rawType > static_cast<int>(ksword::memwb::ValueType::F64))
            {
                return false;
            }
            return m_store->setValueType(id, static_cast<ksword::memwb::ValueType>(rawType));
        }
        // 其余列（含"值"列）不接受 setData：值列的真正提交走 AddressBookPanel 的专用编辑器
        // 与 valueEditRequested 信号，不经过本函数。
        return false;
    }

    Qt::ItemFlags AddressBookModel::flags(const QModelIndex& index) const
    {
        if (!index.isValid())
        {
            return Qt::NoItemFlags;
        }
        Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        switch (index.column())
        {
        case ColumnNote:
            // 备注列由本模型直接落库，走标准的 Qt 编辑流程。
            result |= Qt::ItemIsEditable;
            break;
        case ColumnValueType:
            // 修复 C3：值类型列不再标可编辑——双击/F2 打开的默认委托只是个空 QLineEdit，
            // 什么都不输入直接提交会把 value.toInt() 的失败结果（0）当成新类型，静默把
            // 类型重置成 Hex8。类型只能通过右键"值类型▸"子菜单改（该菜单直接调用
            // setData，不经过这个标志），不再提供"格子内编辑"这条路。
            break;
        case ColumnValue:
        {
            // 修复 C2：只有 Read 状态的值允许进入编辑态——NotRead/Reading/Unreadable/Stale
            // 都是占位符或过期文本，一旦被编辑器当成"用户想改的旧值"提交，就会把这些占位/
            // 过期内容当新值写回目标（尤其是 Stale：换了目标进程后残留的旧文本最危险）。
            const std::uint64_t id = idAt(index);
            const auto cellIt = m_valueCells.find(id);
            const bool readyForEdit = (cellIt != m_valueCells.end()) && (cellIt->second.state == ValueState::Read);
            if (readyForEdit && !isPointerChain(id))
            {
                // 允许触发"进入编辑态"（双击/F2/Enter 都靠这个标志才能叫出编辑器），但
                // setData 对这一列恒返回 false——真正提交由 AddressBookPanel 的专用委托接管，
                // 不经过标准的"编辑器关闭就调用 model->setData"流程。
                result |= Qt::ItemIsEditable;
            }
            break;
        }
        default:
            break;
        }
        return result;
    }

    QVariant AddressBookModel::headerData(const int section, const Qt::Orientation orientation, const int role) const
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        {
            return QAbstractTableModel::headerData(section, orientation, role);
        }
        // 表头标题本身是纯 UI 文案（不是数据），全部经语言包翻译（C12）；与 UI/FlatTableModel.h
        // 的既有做法一致，headerData 用 displayText 而不是 sourceText——两者在当前语言不是
        // zh-CN 时行为相同，区别只在于 displayText 兼容"传入的文本已经是渲染后的译文"这种
        // 场景（本处调用点永远传源文本，实际走的是同一条解析路径）。
        switch (section)
        {
        case ColumnKindIcon: return ks::i18n::displayText(QStringLiteral("类型"));
        case ColumnAddress: return ks::i18n::displayText(QStringLiteral("地址"));
        case ColumnValue: return ks::i18n::displayText(QStringLiteral("值"));
        case ColumnValueType: return ks::i18n::displayText(QStringLiteral("值类型"));
        case ColumnNote: return ks::i18n::displayText(QStringLiteral("备注"));
        case ColumnModuleOffset: return ks::i18n::displayText(QStringLiteral("模块+RVA"));
        case ColumnTarget: return ks::i18n::displayText(QStringLiteral("目标"));
        default: return QVariant();
        }
    }

    void AddressBookModel::setKindFilter(const std::optional<ksword::memwb::EntryKind> kind)
    {
        if (m_kindFilter == kind)
        {
            return;
        }
        m_kindFilter = kind;
        rebuildRows();
    }

    std::optional<ksword::memwb::EntryKind> AddressBookModel::kindFilter() const
    {
        return m_kindFilter;
    }

    AddressBookModel::KindCounts AddressBookModel::kindCounts() const
    {
        return m_counts;
    }

    void AddressBookModel::setValueText(const std::uint64_t id, const QString& text, const ValueState state)
    {
        // 修复 C14：条目已经不在簿里（被删除，或从未存在过的 id）时直接忽略，不写入
        // m_valueCells——否则异步读回的结果晚到时会一直往这张表里累积从不清理的旧 id，
        // 号段被复用后（见 MemoryAddressBook.h 关于 id 复用的注释）旧值还会显示到新条目上。
        const auto entry = m_store != nullptr ? m_store->find(id) : std::nullopt;
        if (!entry.has_value() || entry->pointerChain.has_value())
        {
            return;
        }
        // 修复 C1：文本与状态都跟上一次完全相同时直接跳过，既不写 map 也不发 dataChanged。
        // 常驻监视按 1Hz 喂入是常态，哪怕值没变也会反复调用本函数；若每次都发 dataChanged，
        // Qt 对"正在编辑的索引"收到 dataChanged 会再调一次委托的 setEditorData，用户刚敲的
        // 字就被原样喂回去的旧文本覆盖、还被全选——这正是 C1 的根因。这里的判断只是第一道
        // 防线，第二道在 AddressBookPanel 的 ValueColumnDelegate（只在编辑器刚打开时填一次）。
        const auto existingIt = m_valueCells.find(id);
        if (existingIt != m_valueCells.end()
            && existingIt->second.text == text
            && existingIt->second.state == state)
        {
            return;
        }
        m_valueCells[id] = ValueCell{ text, state };
        const int row = rowForId(id);
        if (row < 0)
        {
            // 条目当前被 kind 过滤掉了（例如正在查看"书签"分段，这是一条"监视"条目）：
            // 值仍然记下来，等用户切回对应分段自然就能看到最新值，这里不必发任何信号。
            return;
        }
        emit dataChanged(index(row, ColumnValue), index(row, ColumnValue));
    }

    AddressBookModel::ValueState AddressBookModel::valueState(const std::uint64_t id) const
    {
        if (isPointerChain(id)) return ValueState::NotRead;
        const auto it = m_valueCells.find(id);
        return it != m_valueCells.end() ? it->second.state : ValueState::NotRead;
    }

    QString AddressBookModel::valueText(const std::uint64_t id) const
    {
        if (isPointerChain(id)) return QString();
        const auto it = m_valueCells.find(id);
        return it != m_valueCells.end() ? it->second.text : QString();
    }

    bool AddressBookModel::isPointerChain(const std::uint64_t id) const
    {
        const auto entry = m_store != nullptr ? m_store->find(id) : std::nullopt;
        return entry.has_value() && entry->pointerChain.has_value();
    }

    std::uint64_t AddressBookModel::idAt(const QModelIndex& index) const
    {
        if (!index.isValid())
        {
            return 0;
        }
        if (index.row() < 0 || static_cast<std::size_t>(index.row()) >= m_rowIds.size())
        {
            return 0;
        }
        return m_rowIds[static_cast<std::size_t>(index.row())];
    }

    QModelIndex AddressBookModel::indexForId(const std::uint64_t id, const int column) const
    {
        const int row = rowForId(id);
        if (row < 0)
        {
            return QModelIndex();
        }
        return index(row, column);
    }

    void AddressBookModel::setTargetDisplayName(const QString& targetKey, const QString& displayName)
    {
        if (displayName.isEmpty())
        {
            m_targetNames.erase(targetKey);
        }
        else
        {
            m_targetNames[targetKey] = displayName;
        }
        // 目标名变化会影响已经显示在表里的"目标"列文本，整表刷新该列最简单也最不容易出错
        // （targetKey 相同的条目可能分布在任意行，没有现成的"受影响行集合"）。
        if (!m_rowIds.empty())
        {
            emit dataChanged(
                index(0, ColumnTarget),
                index(static_cast<int>(m_rowIds.size()) - 1, ColumnTarget));
        }
    }

    // onStoreEntryAdded/Removed/Changed/Reset/BookReloaded/Destroyed、rebuildRows、
    // computeCountsAndRowIds、recomputeCountsOnly、entryForRow、rowForId：全部拆到
    // AddressBookModel.StoreSync.cpp（本文件修复波后超过单文件行数上限）。
}
