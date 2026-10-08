#include "MemoryEditorWidget.h"
#include "MemoryAssembly.h"
#include "HexEditorWidget.h"
#include "X64DbgNavigation.h"
#include "MemoryWorkbench/WorkbenchTextView.h"
#include "MemoryWorkbench/MemoryRowCanvas.h"

#include <QCheckBox>
#include <QComboBox>
#include <QPointer>
#include <algorithm>

namespace ks::ui
{
    MemoryEditorWidget::~MemoryEditorWidget()
    {
        // QObject deletes the child views after C++ members. Release their
        // non-owning provider reference while that member still exists.
        m_disassembly->setBytesProvider(nullptr);
        m_text->setBytesProvider(nullptr);
    }

    void MemoryEditorWidget::initializeInlineAssemblyEditing()
    {
        m_disassembly->setBytesProvider(&m_bytesProvider);
        m_disassembly->setDecodeBackend([](const std::uint8_t* bytes, std::size_t available,
            std::uint64_t address, bool x64) -> std::optional<DecodedRow> {
            const QByteArray input(reinterpret_cast<const char*>(bytes),
                static_cast<qsizetype>(std::min<std::size_t>(available, 15)));
            const auto result = InstructionDecoder::decode(input, address,
                x64 ? DisassemblyArchitecture::X64 : DisassemblyArchitecture::X86, 1);
            if (result.rows.isEmpty() || !result.rows.first().decoded) return std::nullopt;
            const auto& row = result.rows.first();
            return DecodedRow{row.address, row.bytes, row.mnemonic, row.operands, true};
        });
        m_disassembly->setAssembleBackend([](const QString& source, std::uint64_t address, bool x64) {
            const auto result = InstructionAssembler::assemble(source, address,
                x64 ? DisassemblyArchitecture::X64 : DisassemblyArchitecture::X86);
            return WorkbenchAssembleResult{result.success, result.bytes, result.error, result.errorLine};
        });
        connect(m_disassembly, &WorkbenchDisasmView::stageRequested, this,
            [this](quint64 address, const QByteArray& bytes) { stageSnapshotBytes(address, bytes); });
    }

    void MemoryEditorWidget::synchronizeSnapshotProvider()
    {
        m_bytesProvider.setSnapshot(m_base, data(), m_original, m_previousRead,
            architecture() == DisassemblyArchitecture::X64 ? 64 : 32,
            m_highlightChanges->isChecked());
        m_disassembly->setAddressRange(m_base, static_cast<std::uint64_t>(data().size()));
        m_text->setAddressBits(architecture() == DisassemblyArchitecture::X64 ? 64 : 32);
        m_text->setAddressRange(m_base, static_cast<std::uint64_t>(data().size()));
    }

    void MemoryEditorWidget::stageSnapshotBytes(std::uint64_t address, const QByteArray& bytes)
    {
        if (!m_editable || bytes.isEmpty() || !contains(address)) return;
        auto changed = data();
        const auto offset = static_cast<qsizetype>(address - m_base);
        if (bytes.size() > changed.size() - offset) return;
        // The view has already checked the frozen instruction bytes and context.
        // This transaction touches only the shared cache; the host's apply action
        // retains its existing write-before-compare and final-readback gate.
        changed.replace(offset, bytes.size(), bytes);
        const QPointer<MemoryEditorWidget> self(this);
        m_hex->setByteArray(changed, m_base);
        if (!self) return;
        refreshFromHexEditor();
        if (!self) return;
        m_syncing = true;
        m_hex->selectAbsoluteRange(address, address + static_cast<std::uint64_t>(bytes.size() - 1));
        m_syncing = false;
    }

    void MemoryEditorWidget::beginInlineAssemblyEdit()
    {
        if (m_editable) m_disassembly->beginSelectedInstructionEdit();
    }

    void MemoryEditorWidget::setProcessContext(std::uint32_t pid, std::uint64_t createTime100ns)
    {
        const auto creation = pid != 0 && createTime100ns == 0
            ? x64dbg_navigation::ProcessCreateTime100ns(pid) : createTime100ns;
        m_processPid = pid != 0 && creation != 0 ? pid : 0;
        m_processCreateTime100ns = m_processPid != 0 ? creation : 0;
    }
}
