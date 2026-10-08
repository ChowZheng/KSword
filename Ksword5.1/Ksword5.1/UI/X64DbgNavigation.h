#pragma once
#include <QtGlobal>

class QMenu;
class QWidget;

namespace ks::ui::x64dbg_navigation
{
    enum class View { Disassembly, Dump };
    struct Target
    {
        quint32 pid = 0;
        quint64 processCreateTime100ns = 0;
        quint64 address = 0; // 0: show the debugger's current instruction.
        View view = View::Disassembly;
    };

    quint64 ProcessCreateTime100ns(quint32 pid) noexcept;
    // Snapshots must freeze creation time when read, not when a later menu is
    // opened. Zero creation time is accepted only for a current live target.
    void Open(QWidget* owner, const Target& target);
    void AddAction(QMenu* menu, QWidget* owner, const Target& target);
    void Configure(QWidget* owner);
}
