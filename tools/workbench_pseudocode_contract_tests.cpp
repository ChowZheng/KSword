#include "workbench_pseudocode_contract_tests.h"
#include "../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchPseudocodeView.h"
#include "../Ksword5.1/Ksword5.1/UI/CodeTextEdit.h"
#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTest>
#include <QToolButton>
#include <algorithm>
#include <limits>

namespace
{
    // MaskedProvider：只给合成窗口，不依赖任何真实目标；掩码能独立模拟异步与不可读。
    class MaskedProvider final : public ks::ui::IWorkbenchBytesProvider
    {
    public:
        std::uint64_t base = 0x1000; // 合成证据的起始地址。
        std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(32, 0x90);
        std::vector<std::uint8_t> mask = std::vector<std::uint8_t>(32, 1);
        bool available = true;      // 整个来源当前是否可取。
        bool malformed = false;     // 模拟适配器长度错误。
        mutable std::uint64_t largestRequest = 0; // 断言共享页遵守单次缓存拷贝上限。
        ks::ui::WorkbenchByteWindow FetchWindow(std::uint64_t address, std::uint64_t length) const override
        {
            ks::ui::WorkbenchByteWindow result;
            result.address = address;
            largestRequest = std::max(largestRequest, length);
            if (length > 65536 || !available || address < base || address - base > bytes.size()
                || length > bytes.size() - (address - base)) return result;
            const auto offset = static_cast<std::size_t>(address - base);
            result.ok = true;
            result.bytes.assign(bytes.begin() + offset, bytes.begin() + offset + static_cast<std::size_t>(length));
            result.validMask.assign(mask.begin() + offset, mask.begin() + offset + static_cast<std::size_t>(length));
            if (malformed && !result.validMask.empty()) result.validMask.pop_back();
            return result;
        }
        int AddressBits() const override { return 64; }
        bool HasPreviousRead() const override { return false; }
    };

    // DrainBackend：无效后端目录只触发配置错误，不创建任何分析进程。
    void DrainBackend(ks::ui::WorkbenchPseudocodeView& page)
    {
        for (int i = 0; page.decompiler()->isRunning() && i < 100; ++i) QTest::qWait(2);
        QApplication::processEvents();
    }
}

void RunWorkbenchPseudocodeContractTests(const std::function<void(bool, const char*)>& require)
{
    using namespace ks::ui;
    MaskedProvider provider;
    WorkbenchPseudocodeView page;
    page.setBytesProvider(&provider);
    WorkbenchPseudocodeContext context;
    context.sourceIdentity = QStringLiteral("synthetic-C-source");
    context.revision = 1;
    context.baseAddress = provider.base;
    context.length = provider.bytes.size();
    context.selectedAddress = provider.base;
    page.setContext(context);
    page.findChild<QLineEdit*>(QStringLiteral("memory_decompiler_directory"))->setText(
        QDir::current().filePath(QStringLiteral(".codex-build-logs/not-a-ghidra-runtime")));
    unsigned starts = 0;       // 实际后端启动入口触发次数，不把空白编辑器当拒绝证据。
    unsigned requested = 0;    // 唯一的异步读取请求数。
    QObject::connect(page.decompiler(), &GhidraDecompiler::runningChanged, &page,
        [&](bool running) { if (running) ++starts; });
    QObject::connect(&page, &WorkbenchPseudocodeView::windowRequested, &page,
        [&](quint64 address, quint64 length) {
            ++requested;
            require(address == context.baseAddress && length == context.length,
                "C loading request preserves the exact captured analysis window");
        });

    // 掩码 0、错误长度和未知掩码都不能被补零或误解为有效证据。
    for (const auto invalid : {std::uint8_t{0}, std::uint8_t{3}, std::uint8_t{255}})
    {
        provider.mask[5] = invalid;
        page.startDecompilation();
        require(starts == 0 && requested == 0, "C backend never starts on unavailable or unknown-mask bytes");
    }
    provider.mask[5] = 1;
    provider.malformed = true;
    page.startDecompilation();
    require(starts == 0 && requested == 0, "C backend rejects a malformed validity-mask length");
    provider.malformed = false;
    provider.mask[5] = 2;
    page.startDecompilation();
    page.refreshView();
    page.refreshView();
    require(starts == 0 && requested == 1, "C loading waits without repeated I/O requests or decoding pending bytes");
    provider.mask[5] = 1;
    page.refreshView();
    DrainBackend(page);
    require(starts == 1 && !page.decompiler()->isRunning(), "C pending analysis resumes exactly once after every byte is valid");

    // 合成结果只用于检查 UI 接收票据，真实 Ghidra JSON/进程由既有后端夹具验证。
    DecompilerResult result;
    result.success = true;
    result.code = QStringLiteral("int synthetic_function() { return 7; }");
    result.functionName = QStringLiteral("synthetic_function");
    result.functionAddress = provider.base;
    result.lineAddresses = {provider.base};
    result.lineAddressValid = {true};
    emit page.decompiler()->finished(result);
    require(page.editor()->toPlainText() == result.code, "C view accepts a result matching the frozen identity and bytes");
    std::optional<quint64> located;
    QObject::connect(&page, &WorkbenchPseudocodeView::requestHexLocate, &page,
        [&](quint64 address) { located = address; });
    page.findChild<QPushButton*>(QStringLiteral("memory_pseudocode_locate_hex"))->click();
    require(located == provider.base, "C line navigation uses the same valid source address");
    provider.bytes[3] ^= 1;
    page.refreshView();
    emit page.decompiler()->finished(result);
    require(page.editor()->toPlainText().isEmpty(), "C input edits invalidate the result and suppress late backend delivery");
    page.startDecompilation();
    DrainBackend(page);
    ++context.revision;
    page.setContext(context);
    emit page.decompiler()->finished(result);
    require(page.editor()->toPlainText().isEmpty(), "C source generation changes suppress late results even for identical addresses");

    // 范围回绕和选点越界必须在取字节或后端启动前拒绝。
    const auto beforeBounds = starts;
    context.baseAddress = UINT64_MAX - 1;
    context.selectedAddress = context.baseAddress;
    context.length = 4;
    page.setContext(context);
    page.startDecompilation();
    context.baseAddress = provider.base;
    context.length = provider.bytes.size();
    context.selectedAddress = provider.base + provider.bytes.size();
    page.setContext(context);
    page.startDecompilation();
    require(starts == beforeBounds && requested == 1, "C analysis rejects overflowing ranges and the exclusive end address");
    context.selectedAddress = provider.base;
    context.maximumWindowBytes = 8;
    page.setContext(context);
    page.startDecompilation();
    require(starts == beforeBounds, "C analysis honors the host read-window capacity without silently truncating selections");

    // 快照超过单次提供者容量时分块组成同一输入，末块变更也必须撤销旧结果。
    provider.bytes.resize(2 * 65536 + 37, 0x90);
    provider.mask.resize(provider.bytes.size(), 1);
    context.length = provider.bytes.size();
    context.maximumWindowBytes = 0;
    page.setContext(context);
    page.startDecompilation();
    DrainBackend(page);
    emit page.decompiler()->finished(result);
    require(starts == beforeBounds + 1 && provider.largestRequest <= 65536
        && page.editor()->toPlainText() == result.code,
        "C analysis assembles a complete multi-chunk snapshot within the provider per-fetch capacity");
    provider.bytes.back() ^= 1;
    page.refreshView();
    require(page.editor()->toPlainText().isEmpty(), "C result verification includes edits in the last snapshot chunk");
    provider.bytes.resize(32);
    provider.mask.resize(32);
    context.length = provider.bytes.size();

    // 文件偏移与 VA 单义映射；零填充、虚拟尾部和重叠节都没有可定位的文件字节。
    auto image = std::make_shared<std::vector<std::uint8_t>>(0x100, 0x90);
    FileAnalysisRegion region;
    region.fileOffset = 0x20;
    region.fileSize = 0x20;
    region.rva = 0x1000;
    region.virtualSize = 0x80;
    page.setFileAnalysisContext(image, 0x180000000ULL, {region});
    require(page.fileOffsetToVirtualAddress(0x25) == 0x180001005ULL
        && page.virtualAddressToFileOffset(0x180001005ULL) == 0x25,
        "C PE mapping keeps 64-bit image VAs distinct from source file offsets");
    require(!page.fileOffsetToVirtualAddress(0x45) && !page.virtualAddressToFileOffset(0x180001025ULL),
        "C PE mapping excludes file gaps and the virtual-only section tail");
    page.setFileAnalysisContext(image, 0x180000000ULL, {region, region});
    require(!page.fileOffsetToVirtualAddress(0x25) && !page.virtualAddressToFileOffset(0x180001005ULL),
        "C PE mapping rejects ambiguous overlapping sections");
    region.rva = UINT64_MAX;
    page.setFileAnalysisContext(image, UINT64_MAX - 1, {region});
    require(!page.fileOffsetToVirtualAddress(0x25), "C PE mapping rejects invalid or overflowing RVAs");
    page.openFindPanel();
    require(page.findChild<QWidget*>(QStringLiteral("code_editor_find_panel")) != nullptr,
        "C view reuses the project editor find UI");

    // 等待读页回调可以关闭页面；refreshView 必须在回调返回后停止访问销毁对象。
    provider.mask[5] = 2;
    QPointer<WorkbenchPseudocodeView> retiring = new WorkbenchPseudocodeView;
    context.maximumWindowBytes = 0;
    retiring->setBytesProvider(&provider);
    retiring->setContext(context);
    QObject::connect(retiring, &WorkbenchPseudocodeView::windowRequested, qApp,
        [retiring](quint64, quint64) { delete retiring.data(); });
    retiring->startDecompilation();
    require(retiring.isNull(), "C page permits its host to close it from the pending-window callback");
}
