#include "MemoryAssembly.h"
#include "MemoryAssembly.Core.h"
#include "KernelDisassemblyDialog.h"
#include "../Internationalization/LanguageManager.h"

namespace ks::ui
{
    AssemblyResult InstructionAssembler::assemble(
        const QString& source,
        const std::uint64_t baseAddress,
        const DisassemblyArchitecture architecture)
    {
        const auto encoded = detail::assembleIntel(
            source.toUtf8().toStdString(), baseAddress,
            architecture == DisassemblyArchitecture::X64);
        AssemblyResult result;
        result.success = encoded.success;
        result.errorLine = encoded.errorLine;
        if (encoded.success)
        {
            result.bytes = QByteArray(
                reinterpret_cast<const char*>(encoded.bytes.data()),
                static_cast<qsizetype>(encoded.bytes.size()));
            return result;
        }
        QString message;
        switch (encoded.error)
        {
        case detail::AssemblyError::EmptySource: message = QStringLiteral("请输入汇编指令。"); break;
        case detail::AssemblyError::SourceTooLarge: message = QStringLiteral("汇编源文本或行数超过限制。"); break;
        case detail::AssemblyError::InvalidLabel: message = QStringLiteral("标签名称无效。"); break;
        case detail::AssemblyError::DuplicateLabel: message = QStringLiteral("标签重复定义。"); break;
        case detail::AssemblyError::UnknownLabel: message = QStringLiteral("标签未定义。"); break;
        case detail::AssemblyError::UnknownMnemonic: message = QStringLiteral("未知的指令助记符。"); break;
        case detail::AssemblyError::InvalidOperands: message = QStringLiteral("操作数格式或数量无效。"); break;
        case detail::AssemblyError::InvalidNumber: message = QStringLiteral("数值格式无效或超出范围。"); break;
        case detail::AssemblyError::InvalidMemory: message = QStringLiteral("内存寻址表达式无效。"); break;
        case detail::AssemblyError::AmbiguousMemorySize: message = QStringLiteral("内存操作数大小不明确，请指定 byte/word/dword/qword ptr 等大小。"); break;
        case detail::AssemblyError::InvalidInstruction: message = QStringLiteral("当前架构无法编码该指令、操作数或目标地址。"); break;
        case detail::AssemblyError::AddressOverflow: message = QStringLiteral("汇编地址超出当前架构的地址范围。"); break;
        case detail::AssemblyError::OutputTooLarge: message = QStringLiteral("汇编结果超过 64 KiB 限制。"); break;
        case detail::AssemblyError::UnstableLabels: message = QStringLiteral("标签地址无法收敛，请为分支指定 short 或 near。"); break;
        case detail::AssemblyError::UnsupportedSyntax: message = QStringLiteral("不支持该汇编语法；请使用 Intel 指令、局部标签和分号注释。"); break;
        default: message = QStringLiteral("汇编失败。"); break;
        }
        result.error = ks::i18n::LanguageManager::instance().sourceText(message);
        if (!encoded.detail.empty())
            result.error += QStringLiteral(" [")
                + QString::fromStdString(encoded.detail) + QChar(']');
        return result;
    }
}
