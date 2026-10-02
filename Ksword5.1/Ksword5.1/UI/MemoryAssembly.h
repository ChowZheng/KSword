#pragma once

#include <QByteArray>
#include <QString>
#include <cstdint>

namespace ks::ui
{
    enum class DisassemblyArchitecture : int;

    struct AssemblyResult
    {
        QByteArray bytes;
        QString error;
        int errorLine = 0;
        bool success = false;
    };

    class InstructionAssembler final
    {
    public:
        static AssemblyResult assemble(
            const QString& source,
            std::uint64_t baseAddress,
            DisassemblyArchitecture architecture);
    };
}
