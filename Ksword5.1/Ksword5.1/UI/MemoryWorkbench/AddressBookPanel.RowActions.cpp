// AddressBookPanel.RowActions.cpp
// 作用：AddressBookPanel 的"按行为的"实现——targetRowId 的选区/当前格裁决规则、复制/跳转/
// 反汇编/升级/删除/编辑备注/值类型批改等动作、selectedIds 的视觉顺序、键盘快捷键转发、
// D6 修复的整表 reset 前后选区保存/恢复、载入失败横幅文案拼装。从 AddressBookPanel.cpp
// 拆出来（原文件在本轮修复后超过 AGENTS.md 的单文件行数上限），构造/布局/主题/kind 分段
// 那部分逻辑留在原文件，这里只放"一次用户操作该落到哪一条、该发哪个信号"这类行为代码。

#include "AddressBookPanel.h"

#include "../../Internationalization/LanguageManager.h"
#include "HexViewWidgets.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QItemSelectionModel>
#include <QSortFilterProxyModel>

#include <algorithm>
#include <utility>

namespace ks::ui
{
    std::uint64_t AddressBookPanel::targetRowId() const
    {
        if (m_model.isNull())
        {
            return 0;
        }
        const std::vector<std::uint64_t> selection = selectedIds();
        if (selection.size() == 1)
        {
            // 选区恰好一条：没有歧义，就是它——这是最常见也最该信任的情形。
            return selection.front();
        }
        const std::uint64_t currentId = m_model->idAt(sourceIndexForCurrent());
        if (currentId == 0)
        {
            return 0;
        }
        if (selection.empty())
        {
            // 无选区：退化为当前格（例如只移动过焦点、从未真正点选过一行）。
            return currentId;
        }
        // 修复 C7：选区有多条时，当前格必须落在选区里才认它——否则会出现审核报告描述的
        // 场景：Ctrl+点击先选中 B 再取消选中 B，选区仍是 {A}，但当前格停在 B；这时
        // "跳转/反汇编/F2/复制"不该落到没被选中的 B 上。
        const bool currentInSelection =
            std::find(selection.begin(), selection.end(), currentId) != selection.end();
        return currentInSelection ? currentId : 0;
    }

    void AddressBookPanel::captureSelectionForResetRestore()
    {
        // 修复 D6：modelAboutToBeReset 在 beginResetModel() 真正清空行结构之前发出，这时
        // selectedIds()/当前格还能正常读到"重建前最后一刻"的状态，按 id 记下来——只有 id
        // 能跨越整表重建存活，行号在重建后完全没有意义。
        m_pendingSelectionRestore = selectedIds();
        m_pendingCurrentRestore = m_model.isNull() ? 0 : m_model->idAt(sourceIndexForCurrent());
    }

    void AddressBookPanel::restoreSelectionAfterReset()
    {
        if (m_pendingSelectionRestore.empty() && m_pendingCurrentRestore == 0)
        {
            return;  // 重建前本就没有选区/当前格，没什么要恢复的。
        }
        const std::vector<std::uint64_t> idsToRestore = std::move(m_pendingSelectionRestore);
        const std::uint64_t currentIdToRestore = m_pendingCurrentRestore;
        m_pendingSelectionRestore.clear();
        m_pendingCurrentRestore = 0;
        if (m_model.isNull() || m_proxy == nullptr || m_view == nullptr || m_view->selectionModel() == nullptr)
        {
            return;
        }
        // 逐个按 id 换算回重建后的新行号；真的不在了（这一轮批量变更恰好把它删掉了）就
        // 跳过，不是缺陷——D6 要保的是"没被这次变更动过的条目不要无辜丢选区"，不是"被删
        // 掉的条目也要凭空变出一行"。
        QItemSelection restoredSelection;
        for (const std::uint64_t id : idsToRestore)
        {
            const QModelIndex sourceIndex = m_model->indexForId(id, AddressBookModel::ColumnKindIcon);
            if (!sourceIndex.isValid())
            {
                continue;
            }
            const QModelIndex proxyIndex = m_proxy->mapFromSource(sourceIndex);
            if (proxyIndex.isValid())
            {
                restoredSelection.select(proxyIndex, proxyIndex);
            }
        }
        if (!restoredSelection.isEmpty())
        {
            m_view->selectionModel()->select(
                restoredSelection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
        if (currentIdToRestore != 0)
        {
            const QModelIndex currentSourceIndex =
                m_model->indexForId(currentIdToRestore, AddressBookModel::ColumnAddress);
            if (currentSourceIndex.isValid())
            {
                const QModelIndex currentProxyIndex = m_proxy->mapFromSource(currentSourceIndex);
                if (currentProxyIndex.isValid())
                {
                    // 必须用 selectionModel()->setCurrentIndex(index, NoUpdate)，不能用
                    // QAbstractItemView::setCurrentIndex(index) 这个便捷重载——后者在没有
                    // 原始事件可查时会按 ExtendedSelection 的默认选择命令处理，对"这一行
                    // 刚好已经被上面的 select() 选中"的情况会把它 Toggle 掉，反而取消选中
                    // （与 D10 踩到的是同一个坑）。这里只想恢复"当前格"，不想再碰一次选区。
                    m_view->selectionModel()->setCurrentIndex(currentProxyIndex, QItemSelectionModel::NoUpdate);
                }
            }
        }
    }

    QString AddressBookPanel::rowTextForId(const std::uint64_t id) const
    {
        if (m_model.isNull())
        {
            return QString();
        }
        const QModelIndex sourceIndex = m_model->indexForId(id, AddressBookModel::ColumnKindIcon);
        if (!sourceIndex.isValid())
        {
            return QString();
        }
        const int row = sourceIndex.row();
        // 只拼当前可见的列（图标列没有文字可复制，恒跳过），顺序即物理列顺序；
        // 用户看到什么就复制什么，与"复制地址/模块+偏移/值"三个单列动作的语义一致。
        QStringList parts;
        for (int column = 0; column < static_cast<int>(AddressBookModel::ColumnCount); ++column)
        {
            if (column == AddressBookModel::ColumnKindIcon || (m_view != nullptr && m_view->isColumnHidden(column)))
            {
                continue;
            }
            parts << m_model->data(m_model->index(row, column), Qt::DisplayRole).toString();
        }
        return parts.join(QStringLiteral("\t"));
    }

    void AddressBookPanel::jumpCurrentRow()
    {
        // 修复 C7：改用 targetRowId()（选区优先、当前格必须落在选区内），不再直接读
        // sourceIndexForCurrent()——否则"右键 A、跳转却跳到 B"这类选区与当前格不一致的
        // 场景会把动作错发到没被选中的行上。
        const std::uint64_t id = targetRowId();
        if (id == 0)
        {
            return;
        }
        emit jumpRequested(static_cast<quint64>(id));
    }

    void AddressBookPanel::openDisassemblyCurrentRow()
    {
        const std::uint64_t id = targetRowId();  // 修复 C7，理由同 jumpCurrentRow。
        if (id == 0)
        {
            return;
        }
        emit openDisassemblyRequested(static_cast<quint64>(id));
    }

    void AddressBookPanel::editPointerChainById(const std::uint64_t id)
    {
        if (!m_model.isNull() && m_model->isPointerChain(id))
        {
            emit pointerChainEditRequested(static_cast<quint64>(id));
        }
    }

    void AddressBookPanel::resolvePointerChainById(const std::uint64_t id)
    {
        if (!m_model.isNull() && m_model->isPointerChain(id))
        {
            emit pointerChainResolveRequested(static_cast<quint64>(id));
        }
    }

    void AddressBookPanel::promoteSelection(const ksword::memwb::EntryKind newKind)
    {
        // Promote/值类型改动作用于"整个选区"，不是"当前格"——与右键菜单按 hasSelection
        // 使能的判据一致，不受 C7 的 targetRowId 规则约束。
        for (const std::uint64_t id : selectedIds())
        {
            if (!m_model.isNull() && m_model->isPointerChain(id)
                && newKind != ksword::memwb::EntryKind::Bookmark)
            {
                continue;
            }
            emit promoteRequested(static_cast<quint64>(id), newKind);
        }
    }

    void AddressBookPanel::removeSelection()
    {
        const std::vector<std::uint64_t> ids = selectedIds();
        if (ids.empty())
        {
            return;
        }
        QList<quint64> idList;
        idList.reserve(static_cast<qsizetype>(ids.size()));
        for (const std::uint64_t id : ids)
        {
            idList.push_back(static_cast<quint64>(id));
        }
        emit removeRequested(idList);
    }

    void AddressBookPanel::editNoteCurrentRow()
    {
        if (m_model.isNull() || m_proxy == nullptr || m_view == nullptr)
        {
            return;
        }
        // 修复 C7：用 targetRowId() 决定编辑哪一条的备注，不再直接读当前格。
        const std::uint64_t id = targetRowId();
        if (id == 0)
        {
            return;
        }
        if (m_view->isColumnHidden(AddressBookModel::ColumnNote))
        {
            // 备注列此刻不可见（例如正显示预设 B）：不强行切列，什么都不做——
            // 用户切到预设 A 或自定义显示备注列后再按 F2 即可。
            return;
        }
        const QModelIndex noteSource = m_model->indexForId(id, AddressBookModel::ColumnNote);
        const QModelIndex noteProxy = m_proxy->mapFromSource(noteSource);
        if (!noteProxy.isValid())
        {
            return;
        }
        m_view->setCurrentIndex(noteProxy);
        m_view->edit(noteProxy);
    }

    void AddressBookPanel::copyCurrentRowField(const CopyField field)
    {
        // previewCopyText 不碰剪贴板，真正的系统剪贴板写入只在这一个函数里发生。
        const QString text = previewCopyText(field);
        if (text.isEmpty())
        {
            return;
        }
        QGuiApplication::clipboard()->setText(text);
    }

    QString AddressBookPanel::previewCopyText(const CopyField field) const
    {
        if (m_model.isNull())
        {
            return QString();
        }
        // 修复 C7：四种复制字段统一按 targetRowId() 取行，不再直接读当前格——否则"选区
        // 是 A、当前格在 B"时，右键"复制地址"会复制到 B 的地址而不是被选中的 A 的地址。
        const std::uint64_t id = targetRowId();
        if (id == 0)
        {
            return QString();
        }
        switch (field)
        {
        case CopyField::Address:
            return m_model->data(m_model->indexForId(id, AddressBookModel::ColumnAddress), Qt::DisplayRole).toString();
        case CopyField::ModuleOffset:
            return m_model->data(
                m_model->indexForId(id, AddressBookModel::ColumnModuleOffset), Qt::DisplayRole).toString();
        case CopyField::Value:
            // 修复可疑点 #4：只有 Read 状态才复制真实值；NotRead/Reading/Unreadable/Stale
            // 都只是占位符或过期文本，直接取 DisplayRole 会把"读取中…"/"不可读"这类占位
            // 字样原样复制出去，对使用者没有意义——这里直接返回空串，copyCurrentRowField
            // 看到空串就不会写入剪贴板（菜单侧也会据此置灰该动作并给出提示）。
            if (m_model->valueState(id) != AddressBookModel::ValueState::Read)
            {
                return QString();
            }
            return m_model->valueText(id);
        case CopyField::Row:
            return rowTextForId(id);
        }
        return QString();
    }

    std::vector<std::uint64_t> AddressBookPanel::selectedIds() const
    {
        std::vector<std::uint64_t> ids;
        if (m_model.isNull() || m_view == nullptr || m_view->selectionModel() == nullptr || m_proxy == nullptr)
        {
            return ids;
        }
        // 修复 D8：QItemSelectionModel::selectedRows() 给出的顺序是"依次选中的顺序"（例如
        // 先 Ctrl+点第 3 行、再点第 1 行、再点第 2 行，返回的就是 [3,1,2]），不是屏幕上看到
        // 的视觉顺序；Ctrl+C 多选复制、"删除"等按这个顺序处理，粘贴/查看结果时行序会和
        // 屏幕上看到的顺序不一致，容易让人以为复制错了行。这里先把"代理行号 + id"一起收
        // 集，再按代理行号升序排列，返回的顺序即视觉顺序（与当前排序/过滤状态一致）。
        std::vector<std::pair<int, std::uint64_t>> rowsAndIds;
        const QModelIndexList selectedRows = m_view->selectionModel()->selectedRows();
        rowsAndIds.reserve(static_cast<std::size_t>(selectedRows.size()));
        for (const QModelIndex& proxyIndex : selectedRows)
        {
            const std::uint64_t id = m_model->idAt(m_proxy->mapToSource(proxyIndex));
            if (id != 0)
            {
                rowsAndIds.emplace_back(proxyIndex.row(), id);
            }
        }
        std::sort(rowsAndIds.begin(), rowsAndIds.end(),
            [](const std::pair<int, std::uint64_t>& lhs, const std::pair<int, std::uint64_t>& rhs) {
                return lhs.first < rhs.first;
            });
        ids.reserve(rowsAndIds.size());
        for (const std::pair<int, std::uint64_t>& rowAndId : rowsAndIds)
        {
            ids.push_back(rowAndId.second);
        }
        return ids;
    }

    void AddressBookPanel::setValueTypeForSelection(const ksword::memwb::ValueType valueType)
    {
        if (m_model.isNull())
        {
            return;
        }
        // 优先对整个选区生效；选区为空（例如只是单击过某一格但没有整行选中）时退化为
        // 只对当前行生效，避免菜单点了"值类型"却什么都没发生——这一支是"选区为空"的
        // 退化路径，不受 C7 的 targetRowId 规则约束（那条规则专门处理"选区非空但当前格
        // 跑到选区外"的歧义，这里选区本就是空的，没有歧义）。
        std::vector<std::uint64_t> ids = selectedIds();
        if (ids.empty())
        {
            const std::uint64_t currentId = m_model->idAt(sourceIndexForCurrent());
            if (currentId != 0)
            {
                ids.push_back(currentId);
            }
        }
        for (const std::uint64_t id : ids)
        {
            const QModelIndex sourceIndex = m_model->indexForId(id, AddressBookModel::ColumnValueType);
            if (sourceIndex.isValid())
            {
                m_model->setData(sourceIndex, static_cast<int>(valueType), Qt::EditRole);
            }
        }
    }

    QString AddressBookPanel::editorInitialText(const QModelIndex& sourceIndex) const
    {
        if (m_model.isNull())
        {
            return QString();
        }
        const std::uint64_t id = m_model->idAt(sourceIndex);
        if (id == 0)
        {
            return QString();
        }
        return m_model->valueText(id);
    }

    void AddressBookPanel::commitValueEdit(const QModelIndex& sourceIndex, const QString& text)
    {
        if (m_model.isNull())
        {
            return;
        }
        const std::uint64_t id = m_model->idAt(sourceIndex);
        if (id == 0)
        {
            return;
        }
        if (m_model->isPointerChain(id))
        {
            return;
        }
        const int rawType = m_model->data(sourceIndex, AddressBookModel::ValueTypeRole).toInt();
        emit valueEditRequested(static_cast<quint64>(id), static_cast<ksword::memwb::ValueType>(rawType), text);
    }

    void AddressBookPanel::handleDeleteKey()
    {
        removeSelection();
    }

    void AddressBookPanel::handleF2Key()
    {
        editNoteCurrentRow();
    }

    void AddressBookPanel::handleEnterKey()
    {
        jumpCurrentRow();
    }

    void AddressBookPanel::handleCopyShortcut()
    {
        // 修复可疑点 #4：多选时 Ctrl+C 应该复制"所选各行"，不是只复制单独一行。单选或
        // 无选区时保持原有语义（退化为 copyCurrentRowField，内部走 targetRowId()）。
        const std::vector<std::uint64_t> ids = selectedIds();
        if (ids.size() <= 1)
        {
            copyCurrentRowField(CopyField::Row);
            return;
        }
        QStringList rows;
        for (const std::uint64_t id : ids)
        {
            const QString rowText = rowTextForId(id);
            if (!rowText.isEmpty())
            {
                rows << rowText;
            }
        }
        if (!rows.isEmpty())
        {
            QGuiApplication::clipboard()->setText(rows.join(QStringLiteral("\n")));
        }
    }

    void AddressBookPanel::showLoadFailure(const QString& message, const QString& backupPath)
    {
        if (message.isEmpty())
        {
            m_loadFailureLabel->clearMessage();
            m_loadFailureLabel->hide();
            return;
        }
        // "载入失败：%1" 是既有词条；backupPath 非空时追加一句新的说明（C4：Store 把坏
        // 文件改名备份后的位置），两句话分别经语言包翻译再拼接。
        // 修复 D2：之前用 "\n" 换行把两句拼成两行，但 HexViewMessageLabel 是单行高度、
        // 单行省略绘制的控件（见 HexViewWidgets.h 的冻结接口说明，本面板不能去改它）——
        // 换行符被当场吃掉下半截，备份路径这句关键信息完全不可见，只有悬停提示里才有。
        // 改成不换行、把备份路径这句放在最前面：单行控件的省略号只砍文本尾部，哪怕窄侧栏
        // 下整句被省略，"备份在哪"这条最该让用户看到的信息也不会被砍掉；两句之间用一个
        // 空格分隔，不引入新词条。
        const QString failureSentence = ks::i18n::sourceText(QStringLiteral("载入失败：%1")).arg(message);
        QString text = failureSentence;
        if (!backupPath.isEmpty())
        {
            const QString backupSentence = ks::i18n::sourceText(
                QStringLiteral("已将无法解析的原文件备份为 %1，请核实内容后再继续操作。")).arg(backupPath);
            text = backupSentence + QStringLiteral(" ") + failureSentence;
        }
        m_loadFailureLabel->setMessage(HexViewMessageLabel::Kind::Error, text);
        m_loadFailureLabel->show();
    }
}
