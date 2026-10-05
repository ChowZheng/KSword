#include "MemoryEditorWidget.h"
#include "MemoryAssembly.h"
#include "HexEditorWidget.h"
#include "../Internationalization/LanguageManager.h"

#include <QLabel>
#include <QLineEdit>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <functional>
#include <utility>

namespace ks::ui
{
    namespace
    {
        // InlineAssemblyDelegate 将整条指令放入单行编辑框，提交交给缓存事务处理。
        // createEditor/commitEditor 分别接收父控件/模型行及编辑框；不直接写入表格模型。
        class InlineAssemblyDelegate final : public QStyledItemDelegate
        {
        public:
            using CreateEditor = std::function<QLineEdit*(QWidget*, const QModelIndex&)>;
            using CommitEditor = std::function<void(QLineEdit*)>;

            InlineAssemblyDelegate(QObject* parent, CreateEditor createEditor, CommitEditor commitEditor)
                : QStyledItemDelegate(parent), m_createEditor(std::move(createEditor)),
                  m_commitEditor(std::move(commitEditor))
            {
            }

            QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                const QModelIndex& index) const override
            {
                return m_createEditor(parent, index);
            }

            void setEditorData(QWidget*, const QModelIndex&) const override
            {
                // 创建时已装入完整汇编文本，不能被默认逻辑改回单独的指令或操作数列。
            }

            void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                const QModelIndex&) const override
            {
                // geometry 跨指令和操作数两列，避免完整汇编文本被狭窄的指令列裁剪。
                auto geometry = option.rect;
                const auto* table = qobject_cast<QTableWidget*>(parent());
                if (table != nullptr)
                {
                    geometry.setLeft(table->columnViewportPosition(2));
                    geometry.setRight(table->viewport()->width() - 1);
                }
                editor->setGeometry(geometry);
            }

            void setModelData(QWidget* editor, QAbstractItemModel*, const QModelIndex&) const override
            {
                // lineEditor 为当前行的输入框；只有生产汇编器校验通过才能暂存字节。
                auto* lineEditor = qobject_cast<QLineEdit*>(editor);
                if (lineEditor != nullptr)
                {
                    m_commitEditor(lineEditor);
                }
            }

        private:
            CreateEditor m_createEditor; // 创建输入框并冻结快照上下文。
            CommitEditor m_commitEditor; // 校验单条指令并排队提交缓存。
        };
    }

    // 初始化委托与点击入口；输入来自当前快照，输出只进入 Hex 编辑缓存。
    void MemoryEditorWidget::initializeInlineAssemblyEditing()
    {
        // createEditor 为每次编辑冻结地址、架构、快照代次和完整原指令字节。
        const auto createEditor = [this](QWidget* parent, const QModelIndex& index) -> QLineEdit* {
            if (!m_editable || !index.isValid() || index.column() < 2)
            {
                return nullptr;
            }
            const auto* addressCell = m_instructions->item(index.row(), 0);
            const auto* mnemonicCell = m_instructions->item(index.row(), 2);
            const auto* operandCell = m_instructions->item(index.row(), 3);
            if (addressCell == nullptr || mnemonicCell == nullptr || operandCell == nullptr
                || !mnemonicCell->data(Qt::UserRole + 42).toBool())
            {
                return nullptr;
            }
            // offset 为快照偏移；decoded 校验当前缓存仍在完整指令边界上。
            const auto offset = addressCell->data(Qt::UserRole + 41).toULongLong();
            const auto snapshot = data();
            if (offset >= static_cast<qulonglong>(snapshot.size()))
            {
                return nullptr;
            }
            const auto decoded = InstructionDecoder::decode(snapshot.mid(static_cast<qsizetype>(offset), 15),
                m_base + offset, architecture(), 1);
            if (decoded.rows.isEmpty() || !decoded.rows.first().decoded)
            {
                return nullptr;
            }
            // editor 保留整条汇编输入；属性携带冻结上下文以防异步刷新后提交旧编辑。
            auto* editor = new QLineEdit(parent);
            editor->setObjectName(QStringLiteral("memory_inline_assembly"));
            editor->setFont(m_instructions->font());
            editor->setText((mnemonicCell->text() + QLatin1Char(' ') + operandCell->text()).trimmed());
            editor->setProperty("inline_source", editor->text());
            editor->setProperty("inline_revision", QVariant::fromValue<qulonglong>(m_snapshotRevision));
            editor->setProperty("inline_base", QVariant::fromValue<qulonglong>(m_base));
            editor->setProperty("inline_offset", QVariant::fromValue<qulonglong>(offset));
            editor->setProperty("inline_arch", static_cast<int>(architecture()));
            editor->setProperty("inline_bytes", decoded.rows.first().bytes);
            editor->selectAll();
            return editor;
        };

        // commitEditor 编译完整输入，错误只显示在页内；取消和无变化输入不改缓存。
        const auto commitEditor = [this](QLineEdit* editor) {
            const auto source = editor->text().trimmed();
            if (source == editor->property("inline_source").toString())
            {
                return;
            }
            const auto revision = editor->property("inline_revision").toULongLong();
            const auto base = editor->property("inline_base").toULongLong();
            const auto offset = editor->property("inline_offset").toULongLong();
            const auto arch = static_cast<DisassemblyArchitecture>(editor->property("inline_arch").toInt());
            const auto oldBytes = editor->property("inline_bytes").toByteArray();
            if (!m_editable || revision != m_snapshotRevision || base != m_base || arch != architecture())
            {
                m_decodeStatus->setText(ks::i18n::sourceText(QStringLiteral("快照或指令架构已变化，已取消行内编辑。")));
                return;
            }
            // result 使用生产汇编器和真实地址；错误输入绝不形成可应用的补丁。
            const auto result = InstructionAssembler::assemble(source, base + offset, arch);
            if (!result.success)
            {
                m_decodeStatus->setText(ks::i18n::sourceText(QStringLiteral("第 %1 行：%2"))
                    .arg(result.errorLine).arg(result.error));
                return;
            }
            const auto decoded = InstructionDecoder::decode(result.bytes, base + offset, arch, 2);
            if (decoded.rows.size() != 1 || !decoded.rows.first().decoded
                || decoded.rows.first().bytes.size() != result.bytes.size())
            {
                m_decodeStatus->setText(ks::i18n::sourceText(QStringLiteral("行内编辑只接受一条完整指令；多行汇编请使用右键汇编编辑。")));
                return;
            }
            if (result.bytes.size() > oldBytes.size())
            {
                m_decodeStatus->setText(ks::i18n::sourceText(QStringLiteral("新指令为 %1 字节，超出原指令的 %2 字节；请右键汇编编辑并明确调整覆盖长度。"))
                    .arg(result.bytes.size()).arg(oldBytes.size()));
                return;
            }
            // payload 只覆盖冻结的原指令；较短指令以 NOP 填满，保留相邻指令边界。
            auto payload = result.bytes;
            payload.append(QByteArray(oldBytes.size() - payload.size(), static_cast<char>(0x90)));
            // 延迟到委托关闭后再重建行，避免在 setModelData 栈内销毁正在编辑的单元格。
            QTimer::singleShot(0, this, [this, revision, base, offset, arch, oldBytes, payload]() {
                auto changed = data();
                if (!m_editable || revision != m_snapshotRevision || base != m_base || arch != architecture()
                    || offset >= static_cast<qulonglong>(changed.size())
                    || changed.mid(static_cast<qsizetype>(offset), oldBytes.size()) != oldBytes)
                {
                    m_decodeStatus->setText(ks::i18n::sourceText(QStringLiteral("快照或指令架构已变化，已取消行内编辑。")));
                    return;
                }
                changed.replace(static_cast<qsizetype>(offset), payload.size(), payload);
                m_hex->setByteArray(changed, m_base);
                refreshFromHexEditor();
                jumpToAddress(base + offset);
                m_decodeStatus->setText(ks::i18n::sourceText(QStringLiteral("已暂存：%1 → %2；使用页面的应用差异按钮写回。"))
                    .arg(QString::fromLatin1(oldBytes.toHex(' ').toUpper()))
                    .arg(QString::fromLatin1(payload.toHex(' ').toUpper())));
            });
        };
        // 两个汇编列均可单击输入完整指令；地址和字节列保持只读。
        m_instructions->setItemDelegate(new InlineAssemblyDelegate(m_instructions, createEditor, commitEditor));
        m_instructions->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
        connect(m_instructions, &QTableWidget::cellClicked, this, [this](int row, int column) {
            if (column >= 2)
            {
                beginInlineAssemblyEdit(row, column);
            }
        });
    }

    // 以指定行/列启动非模态编辑；只读或无效行返回，右键仍走原有预览对话框。
    void MemoryEditorWidget::beginInlineAssemblyEdit(int row, int column)
    {
        if (!m_editable || row < 0 || column < 2 || column > 3)
        {
            return;
        }
        auto* cell = m_instructions->item(row, column);
        if (cell != nullptr && (cell->flags() & Qt::ItemIsEditable))
        {
            m_instructions->setCurrentCell(row, column);
            m_instructions->editItem(cell);
        }
    }
}
