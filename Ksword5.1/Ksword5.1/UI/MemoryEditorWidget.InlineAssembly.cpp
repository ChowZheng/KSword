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
        const int addressBits = m_addressKind == SnapshotAddressKind::FileOffset
            || architecture() == DisassemblyArchitecture::X64 ? 64 : 32;
        m_bytesProvider.setSnapshot(m_base, data(), m_original, m_previousRead,
            addressBits, m_highlightChanges->isChecked());
        m_disassembly->setAddressRange(m_base, static_cast<std::uint64_t>(data().size()));
        m_text->setAddressBits(addressBits);
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
        if (m_addressKind == SnapshotAddressKind::FileOffset)
        {
            m_processPid = 0;
            m_processCreateTime100ns = 0;
            return;
        }
        // 只接受宿主在读取阶段保留的原身份，不能把当前同号进程授权给旧字节。
        // 文件偏移的早退保持不变；物理/内核/缺身份快照仍不拥有进程导航目标。
        const bool identified = x64dbg_navigation::HasCapturedIdentity(pid, createTime100ns);
        m_processPid = identified ? pid : 0;
        m_processCreateTime100ns = identified ? createTime100ns : 0;
    }
}
