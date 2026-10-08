#include "WorkbenchDisasmView.h"
#include "MemoryRowCanvas.h"
#include "HexViewWidgets.h"
#include "../../Internationalization/LanguageManager.h"
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QScrollBar>
#include <algorithm>

namespace ks::ui
{
    void WorkbenchDisasmView::openFind() { m_findBar->show(); m_findEdit->setFocus(); m_findEdit->selectAll(); }
    void WorkbenchDisasmView::findNext() { findMatch(false); }
    void WorkbenchDisasmView::findPrevious() { findMatch(true); }
    void WorkbenchDisasmView::findMatch(bool backwards)
    {
        const QString needle = m_findEdit->text().trimmed();
        if (needle.isEmpty()) return;
        const int count = m_model->rowCount();
        const int first = backwards ? m_canvas->selectedRow() - 1 : std::max(0, m_canvas->selectedRow() + 1);
        for (int offset = 0; offset < count; ++offset)
        {
            const int index = ((first + (backwards ? -offset : offset)) % count + count) % count;
            const auto row = m_model->rowAt(index);
            if (!row) continue;
            const QString instruction = row->mnemonic + QLatin1Char(' ') + row->operands;
            const QString hex = QString::fromLatin1(row->bytes.toHex(' '));
            if (instruction.contains(needle, Qt::CaseInsensitive) || hex.contains(needle, Qt::CaseInsensitive))
            { m_canvas->setSelectedRow(index); return; }
        }
        m_status->setText(ks::i18n::sourceText(QStringLiteral("当前已读取指令中未找到匹配。")));
        emit statusMessage(m_status->text());
    }
    void WorkbenchDisasmView::invalidateEditContext()
    {
        ++m_editContextRevision;
        cancelInlineEdit();
    }

    void WorkbenchDisasmView::setAddressRange(std::uint64_t base, std::uint64_t length)
    {
        m_addressRange = length && length - 1 <= UINT64_MAX - base
            ? std::optional<std::pair<std::uint64_t, std::uint64_t>>(std::make_pair(base, base + length - 1)) : std::nullopt;
    }

    void WorkbenchDisasmView::setAddressBounds(std::uint64_t first, std::uint64_t last)
    {
        m_addressRange = first <= last ? std::optional<std::pair<std::uint64_t, std::uint64_t>>(std::make_pair(first, last)) : std::nullopt;
    }

    void WorkbenchDisasmView::setArchitectureOverride(bool x64)
    {
        if (m_x64Override && m_x64OverrideValue == x64 && m_archSegmented->currentIndex() == (x64 ? 1 : 0)) return;
        m_x64Override = true;
        m_x64OverrideValue = x64;
        m_programmaticArchChange = true;
        m_archSegmented->setCurrentIndex(x64 ? 1 : 0);
        m_programmaticArchChange = false;
        invalidateEditContext();
        rebuildRows();
        emit architectureChanged(x64);
    }

    void WorkbenchDisasmView::clearArchitectureOverride()
    {
        m_x64Override = false;
        setAddressBits(m_provider ? m_provider->AddressBits() : 64);
    }

    void WorkbenchDisasmView::updateCanvas(bool preserveViewport)
    {
        QVector<MemoryDisplayRow> display;
        // Operand tokenization affects presentation only. Follow targets still use the decoder's
        // single absolute branch operand, never a register value or a guessed memory dereference.
        static const QRegularExpression parts(QStringLiteral("(0x[0-9a-fA-F]+|[A-Za-z][A-Za-z0-9]*|[0-9]+|[^A-Za-z0-9]+)"));
        static const QRegularExpression registers(QStringLiteral("^(?:r(?:[0-9]+[bwd]?|ax|bx|cx|dx|si|di|sp|bp|ip)|e(?:ax|bx|cx|dx|si|di|sp|bp|ip)|[abcd][lhx]|[sd]i|[sb]p|[cdefgs]s|[xyz]mm[0-9]+|st[0-9]*)$"), QRegularExpression::CaseInsensitiveOption);
        for (int i = 0; i < m_model->rowCount(); ++i)
        {
            const auto instruction = m_model->rowAt(i);
            if (!instruction) continue;
            const auto& source = *instruction;
            MemoryDisplayRow row;
            row.address = source.address;
            row.bytes = source.bytes;
            row.validMask.fill(1, source.bytes.size());
            if (m_provider)
            {
                const auto window = m_provider->FetchWindow(source.address, static_cast<std::uint64_t>(source.bytes.size()));
                for (auto kind : window.changeKinds) row.changeKinds.push_back(kind);
            }
            const auto length = static_cast<std::uint64_t>(source.bytes.size());
            row.tokens.push_back({source.mnemonic, source.address, length, source.decoded ? MemoryTokenRole::Mnemonic : MemoryTokenRole::Comment});
            row.tokens.push_back({QStringLiteral(" "), source.address, length, MemoryTokenRole::Plain});
            auto matches = parts.globalMatch(source.operands);
            while (matches.hasNext())
            {
                const QString text = matches.next().captured();
                const auto role = registers.match(text).hasMatch() ? MemoryTokenRole::Register
                    : text.startsWith(QLatin1String("0x")) ? MemoryTokenRole::Address
                    : !text.isEmpty() && text[0].isDigit() ? MemoryTokenRole::Number : MemoryTokenRole::Plain;
                row.tokens.push_back({text, source.address, length, role});
            }
            std::uint64_t target = 0;
            if (tryFollowOperand(source, &target)) row.branchTarget = target;
            display.push_back(std::move(row));
            if (m_browseHistory.empty() || m_browseHistory.back() != source.address) m_browseHistory.push_back(source.address);
        }
        if (display.isEmpty() && m_hasAnchor && m_provider && m_decodeOne)
        {
            const auto unavailable = m_provider->FetchWindow(m_anchor, 64);
            const auto count = std::min(unavailable.bytes.size(), unavailable.validMask.size());
            for (std::size_t offset = 0; offset < count && unavailable.validMask[offset] != 1; ++offset)
            {
                MemoryDisplayRow row;
                row.address = m_anchor + offset;
                row.bytes = QByteArray(1, static_cast<char>(unavailable.bytes[offset]));
                row.validMask.push_back(unavailable.validMask[offset]);
                row.selectable = false;
                row.tokens.push_back({ks::i18n::sourceText(unavailable.validMask[offset] == 2 ? QStringLiteral("正在加载…") : QStringLiteral("不可读")), row.address, 1, MemoryTokenRole::Comment});
                display.push_back(row);
            }
        }
        if (display.isEmpty() || m_model->isEndOfWindowRow(m_model->rowCount() - 1))
        {
            MemoryDisplayRow note;
            note.address = display.isEmpty() ? m_anchor : display.back().address
                + std::min<std::uint64_t>(static_cast<std::uint64_t>(display.back().bytes.size()), UINT64_MAX - display.back().address);
            if (m_addressRange) note.address = std::min(note.address, m_addressRange->second);
            note.selectable = false;
            note.tokens.push_back({display.isEmpty() ? m_status->text() : ks::i18n::sourceText(QStringLiteral("继续滚动以读取下一段")), note.address, 0, MemoryTokenRole::Comment});
            display.push_back(note);
        }
        if (m_browseHistory.size() > 8192) m_browseHistory.erase(m_browseHistory.begin(), m_browseHistory.end() - 8192);
        m_canvas->setAddressBits(isX64() ? 64 : 32);
        m_canvas->setRows(std::move(display), preserveViewport);
    }

    void WorkbenchDisasmView::browseMore(int direction, int lines)
    {
        if (!m_hasAnchor || m_editingActive || !m_provider) return;
        std::uint64_t next = m_anchor;
        if (direction > 0)
        {
            const int first = m_canvas->verticalScrollBar()->value();
            const int index = std::min(m_model->rowCount() - 1, first + std::max(1, lines));
            if (const auto row = m_model->rowAt(index)) next = row->address;
            else if (m_model->rowCount() <= 1 && !m_canvas->rows().isEmpty())
                next = m_canvas->rows()[std::min(static_cast<int>(m_canvas->rows().size()) - 1, first + std::max(1, lines))].address;
            else if (const auto last = m_model->rowAt(m_model->rowCount() - 2))
                if (static_cast<std::uint64_t>(last->bytes.size()) <= UINT64_MAX - last->address) next = last->address + last->bytes.size();
        }
        else
        {
            std::vector<std::uint64_t> earlier;
            for (auto address : m_browseHistory) if (address < m_anchor) earlier.push_back(address);
            std::sort(earlier.begin(), earlier.end());
            earlier.erase(std::unique(earlier.begin(), earlier.end()), earlier.end());
            if (!earlier.empty()) next = earlier[earlier.size() - std::min(earlier.size(), static_cast<std::size_t>(std::max(1, lines)))];
            else next = m_anchor - std::min(m_anchor, static_cast<std::uint64_t>(std::max(1, lines)) * 15);
        }
        if (m_addressRange) next = std::clamp(next, m_addressRange->first, m_addressRange->second);
        if (next == m_anchor) return;
        m_anchor = next;
        emit windowRequested(m_anchor, kDecodeWindowBytes + kLookaheadBytes);
        rebuildRows();
    }

}
