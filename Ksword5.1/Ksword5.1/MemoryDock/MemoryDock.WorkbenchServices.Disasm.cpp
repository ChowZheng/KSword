#include "../Framework.h"

#include "MemoryDock.WorkbenchServices.h"
#include "WorkbenchServicesMapping.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/KernelDisassemblyDialog.h"
#include "../UI/MemoryAssembly.h"

#include <QByteArray>
#include <QLatin1String>
#include <QString>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

// ============================================================
// MemoryDock.WorkbenchServices.Disasm.cpp
// 作用：
// - MakeDecodeBackend：把既有 ks::ui::InstructionDecoder::decode（KernelDisassemblyDialog.h）
//   包装成反汇编视图需要的单条解码回调 ks::ui::DecodeOneFn。
// - MakeAssembleBackend：把既有 ks::ui::InstructionAssembler::assemble（MemoryAssembly.h）
//   包装成 ks::ui::AssembleOneFn，错误行号 errorLine 原样透传。
// - 两个包装都只做类型转换与边界判定，**不重写解码/汇编逻辑**（设计文档不变式 10：旧汇编
//   核心原样）。判据来自纯函数文件 WorkbenchServicesMapping.* 的 AcceptDecodedRow /
//   JudgeAssembleResult。
//
// 汇编核心的边界规则如何保持：
// - 有界：汇编核心自己限制源文本大小与 64 KiB 输出；本包装再核一次输出上限
//   （JudgeAssembleResult），"有界"不依赖核心内部实现不被改动；解码侧每次最多取 15 字节。
// - 失败不返回部分机器码：任何失败（后端失败、成功却无输出、输出超限）都返回空 bytes。
// - 覆盖完整旧指令：新机器码是否超出原指令长度、过短时用 NOP 补齐，由反汇编视图在拿到
//   本回调的结果后判定（WorkbenchDisasmView.Edit.cpp），本包装不放宽、也不绕过。
// ============================================================

namespace svc = ksword::memwb_services_detail;

namespace ks::ui::workbench_dock
{
    DecodeOneFn MakeDecodeBackend()
    {
        // 返回的回调不捕获任何状态，可以被反汇编视图随意拷贝与跨调用持有。
        return [](const std::uint8_t* const bytes,
                  const std::size_t available,
                  const std::uint64_t address,
                  const bool x64) -> std::optional<DecodedRow>
        {
            // 没有可解码的字节：交给调用方退化为单字节 db。
            if (bytes == nullptr || available == 0U)
            {
                return std::nullopt;
            }

            // 只取一条指令可能占用的最多 15 字节：既保证解码有界，也避免每次调用都拷贝
            // 整个窗口。decode 的 maximumInstructions=1，只解第一条。
            const std::size_t take = std::min(available, svc::kMaxInstructionBytes);
            const QByteArray window(reinterpret_cast<const char*>(bytes), static_cast<qsizetype>(take));
            const ks::ui::DisassemblyResult decoded = ks::ui::InstructionDecoder::decode(
                window,
                address,
                x64 ? ks::ui::DisassemblyArchitecture::X64 : ks::ui::DisassemblyArchitecture::X86,
                1U);
            if (decoded.rows.isEmpty())
            {
                return std::nullopt;
            }

            // 只接受 Zydis 解出的指令：既有解码器在 Zydis 解不出首条指令时会退到"有界降级
            // 解码器"，那个解码器靠 ModR/M 启发式猜长度、不区分指令边界（旧缺陷 E-02 的来源）；
            // 单条解码失败的正确处理是让视图把这一个字节标成 db 并从下一字节重同步，
            // 而不是接受一个猜出来的长度。后端名以 "Zydis" 开头是 Zydis 路径的固定标记。
            const ks::ui::DisassemblyRow& row = decoded.rows.front();
            const bool fromZydis = decoded.backendName.startsWith(QLatin1String("Zydis"));
            const std::size_t rowLength = row.bytes.size() > 0
                ? static_cast<std::size_t>(row.bytes.size())
                : 0U;
            if (!svc::AcceptDecodedRow(row.decoded, fromZydis, rowLength, take))
            {
                return std::nullopt;
            }

            DecodedRow out;
            // 地址取调用方传入的值而不是解码器回填的值：视图据此定位这一行，不信任解码器。
            out.address = address;
            out.bytes = row.bytes;
            out.mnemonic = row.mnemonic;
            out.operands = row.operands;
            out.decoded = true;
            return out;
        };
    }

    AssembleOneFn MakeAssembleBackend()
    {
        return [](const QString& source, const std::uint64_t address, const bool x64) -> WorkbenchAssembleResult
        {
            // 默认结果就是"失败、没有机器码、没有行号"，所有失败分支都从这里出发，
            // 保证失败时绝不会带出部分机器码。
            WorkbenchAssembleResult out;

            // 源码、地址与架构原样交给既有汇编核心（含 Intel 语法、局部标签、源文本大小限制）。
            const ks::ui::AssemblyResult assembled = ks::ui::InstructionAssembler::assemble(
                source,
                address,
                x64 ? ks::ui::DisassemblyArchitecture::X64 : ks::ui::DisassemblyArchitecture::X86);

            const std::size_t byteCount = assembled.bytes.size() > 0
                ? static_cast<std::size_t>(assembled.bytes.size())
                : 0U;
            switch (svc::JudgeAssembleResult(assembled.success, byteCount))
            {
            case svc::AssembleRejection::None:
                out.success = true;
                out.bytes = assembled.bytes;
                return out;
            case svc::AssembleRejection::BackendFailed:
                // 后端自己报告的失败：原因与出错行号原样透传（行号 0 表示后端没给）。
                out.error = assembled.error;
                out.errorLine = assembled.errorLine;
                return out;
            case svc::AssembleRejection::EmptyOutput:
                // 协议外情况：报告成功却没有机器码。按失败处理，沿用汇编核心既有的通用失败文案。
                out.error = ks::i18n::sourceText(QStringLiteral("汇编失败。"));
                return out;
            case svc::AssembleRejection::OutputTooLarge:
                // 输出超过 64 KiB 上限：沿用汇编核心既有的同义文案，不返回任何机器码。
                out.error = ks::i18n::sourceText(QStringLiteral("汇编结果超过 64 KiB 限制。"));
                return out;
            }

            // 枚举之外的值（不应出现）：保守按失败处理。
            out.error = ks::i18n::sourceText(QStringLiteral("汇编失败。"));
            return out;
        };
    }
}
