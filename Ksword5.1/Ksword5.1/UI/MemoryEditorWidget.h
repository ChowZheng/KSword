#pragma once

#include "KernelDisassemblyDialog.h"
#include "MemoryEditHistory.Core.h"
#include <QWidget>

class HexEditorWidget;
class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;

namespace ks::ui
{
    struct MemoryEditBlock
    {
        std::uint64_t address = 0;
        QByteArray originalBytes;
        QByteArray bytes;
    };

    // Backend-independent snapshot editor. All edits are staged; the owner alone
    // reads/writes the target and decides when a verified snapshot is committed.
    class MemoryEditorWidget final : public QWidget
    {
        Q_OBJECT
    public:
        explicit MemoryEditorWidget(QWidget* parent = nullptr);
        HexEditorWidget* hexEditor() const;
        // A stable nonempty source identity enables comparison across actual
        // reads. Without one, only the current snapshot's edit baseline is used.
        void setSnapshot(const QByteArray& bytes, std::uint64_t base,
            DisassemblyArchitecture architecture = DisassemblyArchitecture::X64,
            std::uint64_t anchor = 0, const QString& sourceIdentity = QString());
        QByteArray data() const;
        QByteArray originalBytes() const;
        std::uint64_t baseAddress() const;
        DisassemblyArchitecture currentArchitecture() const;
        void setEditable(bool editable);
        bool hasChanges() const;
        QVector<MemoryEditBlock> diffBlocks() const;
        void acceptChanges();
        void discardChanges();
        void clear();
        void refreshFromHexEditor();
        void jumpToAddress(std::uint64_t address);
        void showDisassemblyAt(std::uint64_t address);
        QTableWidget* instructionTable() const;
        std::optional<DisassemblySelection> selectedInstruction() const;
        void undo();
        void redo();

    signals:
        void bytesChanged();
        void currentAddressChanged(std::uint64_t address);

    protected:
        void changeEvent(QEvent* event) override;

    private:
        void rebuildDisassembly();
        void rebuildText();
        void rebuildComparison();
        void renderComparisonPage();
        void updateHighlights();
        void applyHistory(bool forward);
        void updateState();
        void showAssemblyEditor();
        // 配置行内汇编编辑；单击/工具按钮只暂存单条完整指令，不写入真实内存。
        void initializeInlineAssemblyEditing();
        void beginInlineAssemblyEdit(int row, int column = 2);
        void showInstructionMenu(const QPoint& position);
        void selectInstruction(std::uint64_t address);
        std::uint64_t selectedAddress() const;
        bool contains(std::uint64_t address) const;
        DisassemblyArchitecture architecture() const;

        HexEditorWidget* m_hex = nullptr;
        QTabWidget* m_tabs = nullptr;
        QTableWidget* m_instructions = nullptr;
        QTableWidget* m_comparison = nullptr;
        QComboBox* m_comparisonBaseline = nullptr;
        QCheckBox* m_onlyDifferences = nullptr;
        QCheckBox* m_highlightChanges = nullptr;
        QPushButton* m_previousComparison = nullptr;
        QPushButton* m_nextComparison = nullptr;
        QLabel* m_comparisonStatus = nullptr;
        QPlainTextEdit* m_text = nullptr;
        QComboBox* m_textEncoding = nullptr;
        QComboBox* m_architecture = nullptr;
        QLineEdit* m_decodeAddress = nullptr;
        QPushButton* m_assemble = nullptr;
        QPushButton* m_undo = nullptr;
        QPushButton* m_redo = nullptr;
        QLabel* m_status = nullptr;
        QLabel* m_decodeStatus = nullptr;
        QByteArray m_original;
        QByteArray m_observed;
        QByteArray m_previousRead;
        QByteArray m_recentChanges;
        QString m_sourceIdentity;
        detail::MemoryEditHistory m_history;
        QVector<qsizetype> m_comparisonRows;
        qsizetype m_comparisonPage = 0;
        std::uint64_t m_base = 0;
        std::uint64_t m_anchor = 0;
        std::uint64_t m_snapshotRevision = 0;
        bool m_editable = false;
        bool m_syncing = false;
    };
}
