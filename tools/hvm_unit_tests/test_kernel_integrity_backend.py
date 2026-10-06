"""Exercise production kernel-integrity scan and verdict control flow offline.

Extracts the current read, PE-header/cache routing, scan and UI verdict methods
verbatim. Qt widgets, loaded-module discovery, disk/CI preparation and the memory
transport are scripted. These checks prove routing and incomplete-result handling;
they do not prove Qt/MSVC builds, trust-provider behavior or hardware reads.
Run: python tools/hvm_unit_tests/test_kernel_integrity_backend.py
"""

from pathlib import Path
import os
import re
import shutil
import subprocess
import uuid


ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "Ksword5.1/Ksword5.1/KernelDock"


def extract(source, name):
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda match: " " * len(match.group()), source, flags=re.DOTALL)
    match = re.search(
        rf'^[ \t]*(?:const PreparedTrustedImage\*|std::vector<[^>]+>|QString|bool|void)\s+'
        rf'{re.escape(name)}\s*\([^;{{}}]*\)\s*{{', masked, re.MULTILINE)
    if not match:
        raise ValueError(f"Production method not found: {name}")
    cursor, depth = match.end(), 1
    while depth:
        depth += (masked[cursor] == "{") - (masked[cursor] == "}")
        cursor += 1
    return source[match.start():cursor]


QT = r'''
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>
using qsizetype = std::int64_t;
namespace Qt { enum CaseSensitivity { CaseInsensitive }; }
class QString {
public:
    std::string value;
    QString() = default;
    QString(const char* text) : value(text) {}
    QString(const std::string& text) : value(text) {}
    void clear() { value.clear(); }
    bool isEmpty() const { return value.empty(); }
    static std::string lowered(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }
    bool contains(const QString& text, Qt::CaseSensitivity) const {
        return lowered(value).find(lowered(text.value)) != std::string::npos;
    }
    int compare(const QString& text, Qt::CaseSensitivity) const {
        return lowered(value).compare(lowered(text.value));
    }
    QString replaceArg(const std::string& text) const {
        auto result = *this;
        auto pos = result.value.find('%');
        if (pos != std::string::npos) {
            auto end = pos + 1;
            while (end < result.value.size() && std::isdigit(static_cast<unsigned char>(result.value[end]))) ++end;
            result.value.replace(pos, end - pos, text);
        }
        return result;
    }
    template<class T> QString arg(T value) const { return replaceArg(std::to_string(value)); }
    QString arg(const QString& text) const { return replaceArg(text.value); }
    QString& operator+=(const QString& text) { value += text.value; return *this; }
    QString operator+(const QString& text) const { return QString(value + text.value); }
};
#define QStringLiteral(text) QString(text)
class QStringList : public std::vector<QString> {
public:
    bool isEmpty() const { return empty(); }
    QString join(const QString& separator) const {
        QString result;
        for (const auto& text : *this) {
            if (!result.isEmpty()) result += separator;
            result += text;
        }
        return result;
    }
};
class QByteArray {
    std::vector<char> value;
public:
    QByteArray() = default;
    QByteArray(qsizetype size, char fill) : value(static_cast<std::size_t>(size), fill) {}
    qsizetype size() const { return static_cast<qsizetype>(value.size()); }
    const char* constData() const { return value.data(); }
};
'''

PRELUDE = r'''
#include "KernelCleanImageBaseline.h"
#include <iostream>
#include <stdexcept>
#include <limits>
#include <utility>
#define KSWORD_ARK_MEMORY_READ_MAX_BYTES (64U * 1024U)
namespace ks::i18n {
static unsigned calls;
QString sourceText(const QString& text) { ++calls; return text; }
}
namespace fake {
using namespace ksword::memory_backend;
struct Call { MemoryAccessBackend backend; std::uint32_t pid; std::uint64_t address, length; bool strict; };
static std::vector<Call> calls;
static std::function<AccessOutcome(unsigned, std::uint64_t)> read;
static bool enumerationFails, preparationFails, emptySections, dynamicParsed = true;
static unsigned preparations;
static std::atomic_bool* cancellation;
static std::vector<std::uint32_t> dynamicSites;
static std::uint64_t nextBase = 0xFFFFF80000000000ULL;
}
namespace ksword::memory_backend {
QString backendDisplayName(MemoryAccessBackend backend) {
    return backend == MemoryAccessBackend::Hvm ? QString("HVM") : QString("R0");
}
AccessOutcome readVirtual(MemoryAccessBackend backend, const DdmaSession&, std::uint32_t pid,
                         std::uint64_t address, std::uint64_t length, bool strict) {
    fake::calls.push_back({backend, pid, address, length, strict});
    if (fake::read) return fake::read(static_cast<unsigned>(fake::calls.size()), length);
    AccessOutcome result; result.ok = true; result.bytesDone = length;
    result.data = QByteArray(static_cast<qsizetype>(length), 'A'); return result;
}
}
namespace {
thread_local bool g_deferBaselineTranslation = false;
struct LoadedModule { std::uint64_t base; std::uint32_t size; QString name, filePath; };
static std::vector<LoadedModule> moduleScript;
struct PreparedTrustedImage {
    ksword::memory_backend::MemoryAccessBackend backend = ksword::memory_backend::MemoryAccessBackend::StandardDriver;
    LoadedModule module;
    bool diskTrustVerified = true, relocationApplied = true;
    QString sha256;
    std::vector<std::uint8_t> mappedImage;
};
struct ExecutableSection { std::uint32_t rva, size; QString name; };
static constexpr std::uint32_t kDynamicRelocSiteSpan = 8U;
bool enumerateLoadedModules(std::vector<LoadedModule>& modules, QString& error) {
    if (fake::enumerationFails) { error = QString("enumeration-failed"); return false; }
    modules = moduleScript; return true;
}
bool collectDynamicRelocationSites(const PreparedTrustedImage&, std::vector<std::uint32_t>& sites) {
    sites = fake::dynamicSites; return fake::dynamicParsed;
}
std::vector<ExecutableSection> collectExecutableSections(const PreparedTrustedImage&) {
    return fake::emptySections ? std::vector<ExecutableSection>{}
                              : std::vector<ExecutableSection>{{0x10000U, 8U, QString(".text")}};
}
std::vector<std::uint8_t> loadedHeaderBytes(std::uint64_t, QString&, ksword::memory_backend::MemoryAccessBackend);
bool prepareTrustedImage(const LoadedModule& module, bool, PreparedTrustedImage& result,
                         QString& error, ksword::memory_backend::MemoryAccessBackend backend) {
    ++fake::preparations;
    if (fake::preparationFails) { error = QString("preparation-failed"); return false; }
    if (loadedHeaderBytes(module.base, error, backend).empty()) return false;
    result.module = module; result.backend = backend;
    result.mappedImage.assign(module.size, 'A'); return true;
}
}
namespace KswordTheme {
QString WarningHex() { return QString("warning"); }
QString SuccessHex() { return QString("success"); }
QString ErrorHex() { return QString("error"); }
}
static QString kernelText(const char*, const QString& text) { return text; }
struct QLabel {
    QString text, style;
    void setText(const QString& value) { text = value; }
    void setStyleSheet(const QString& value) { style = value; }
};
class KernelTextIntegrityTab {
public:
    std::vector<ks::kernel::KernelTextIntegrityResult> m_results;
    bool m_scanRunning = false, m_scanCancelled = false, m_hvciEvidenceUsable = false, m_hvciEnforcing = false;
    QLabel label;
    QLabel* m_verdictLabel = &label;
    void updateVerdict();
};
using ks::kernel::KernelTextIntegrityResult;
using ks::kernel::KernelTextScanOptions;
using ks::kernel::KernelTextDiffRange;
'''

TESTS = r'''
static unsigned checks;
static void check(bool condition, const char* message) {
    ++checks; if (!condition) throw std::runtime_error(message);
}
static void reset() {
    fake::calls.clear(); fake::read = {}; fake::enumerationFails = false;
    fake::preparationFails = false; fake::emptySections = false; fake::dynamicParsed = true;
    fake::dynamicSites.clear(); fake::preparations = 0; fake::cancellation = nullptr;
    fake::nextBase += 0x20000U;
    moduleScript = {{fake::nextBase, 0x10008U, QString("ntoskrnl.exe"), QString("kernel.exe")}};
}
static ksword::memory_backend::AccessOutcome success(std::uint64_t length) {
    ksword::memory_backend::AccessOutcome result; result.ok = true;
    result.bytesDone = length; result.data = QByteArray(static_cast<qsizetype>(length), 'A'); return result;
}
static void checkVerdict(const KernelTextIntegrityResult& result, bool cancelled = false, bool running = false) {
    KernelTextIntegrityTab tab; tab.m_results = {result};
    tab.m_scanCancelled = cancelled; tab.m_scanRunning = running; tab.updateVerdict();
    check(tab.label.style.value.find("success") == std::string::npos,
          "incomplete, cancelled or active scan cannot produce a success verdict");
}
int main() {
  try {
    using Backend = ksword::memory_backend::MemoryAccessBackend;
    using ks::kernel::KernelCleanImageBaseline;
    std::vector<std::uint8_t> bytes = {9}; QString error;
    reset();
    check(KernelCleanImageBaseline::readKernelBytes(fake::nextBase, 4, bytes, error), "legacy read must keep default R0");
    check(fake::calls.size() == 1 && fake::calls[0].backend == Backend::StandardDriver
          && fake::calls[0].pid == 0 && !fake::calls[0].strict && bytes.size() == 4,
          "default kernel read uses PID 0 and R0");
    reset();
    check(KernelCleanImageBaseline::readKernelBytes(fake::nextBase, 4, bytes, error, Backend::Hvm), "HVM read succeeds");
    check(fake::calls[0].strict && fake::calls[0].backend == Backend::Hvm, "HVM read requires private window");
    for (int kind = 0; kind < 5; ++kind) {
      reset(); fake::read = [kind](unsigned, std::uint64_t length) {
        auto result = success(length);
        if (kind == 0) result.ok = false;
        if (kind == 1) result.partial = true;
        if (kind == 2) --result.bytesDone;
        if (kind == 3) result.data = QByteArray(static_cast<qsizetype>(length - 1), 'A');
        if (kind == 4) result.data = QByteArray(static_cast<qsizetype>(length + 1), 'A');
        result.failureText = QString("original-error"); return result;
      };
      bytes = {9};
      check(!KernelCleanImageBaseline::readKernelBytes(fake::nextBase, 4, bytes, error, Backend::Hvm)
            && bytes.empty() && error.value.find("original-error") != std::string::npos && fake::calls.size() == 1,
            "failed/partial/mismatched reads clear bytes and never retry another backend");
    }
    reset();
    check(!KernelCleanImageBaseline::readKernelBytes(fake::nextBase, 4, bytes, error, Backend::Ddma)
          && fake::calls.empty(), "unsupported backend rejected before access");
    check(!KernelCleanImageBaseline::readKernelBytes(0, 4, bytes, error)
          && !KernelCleanImageBaseline::readKernelBytes(fake::nextBase, 0, bytes, error)
          && !KernelCleanImageBaseline::readKernelBytes(fake::nextBase, 65537, bytes, error)
          && fake::calls.empty(), "invalid ranges rejected before access");
    reset(); g_deferBaselineTranslation = true; ks::i18n::calls = 0;
    check(baselineText(QString("source")).value == "source" && ks::i18n::calls == 0,
          "worker must not query LanguageManager");
    g_deferBaselineTranslation = false; baselineText(QString("source"));
    check(ks::i18n::calls == 1, "UI preserves source translation");
    KernelTextScanOptions options; options.chunkBytes = 4; options.backend = Backend::Hvm;
    reset(); ks::i18n::calls = 0; unsigned callbacks = 0;
    options.onModuleComplete = [&](const auto&) { ++callbacks; };
    auto results = KernelCleanImageBaseline::scanExecutableSections(options);
    check(ks::i18n::calls == 0 && !g_deferBaselineTranslation,
          "scan must not query LanguageManager and must restore other callers' translation behavior");
    check(results.size() == 1 && results[0].complete && results[0].available && results[0].scannedBytes == 8,
          "complete executable section reports full coverage");
    check(results[0].backend == Backend::Hvm && callbacks == 1 && fake::calls.size() == 3,
          "result provenance and module callback preserve selected backend");
    for (const auto& call : fake::calls) check(call.backend == Backend::Hvm && call.strict && call.pid == 0,
                                             "HVM routes PE header and each section block through strict PID 0 reads");
    KernelTextIntegrityTab clean; clean.m_results = results; clean.updateVerdict();
    check(clean.label.style.value.find("success") != std::string::npos, "complete clean result can be success");
    checkVerdict(results[0], true); checkVerdict(results[0], false, true);
    fake::calls.clear(); options.backend = Backend::StandardDriver;
    results = KernelCleanImageBaseline::scanExecutableSections(options);
    check(fake::preparations == 2 && fake::calls.size() == 3 && fake::calls[0].backend == Backend::StandardDriver,
          "baseline identity cache cannot cross HVM/R0 provenance");
    fake::calls.clear(); results = KernelCleanImageBaseline::scanExecutableSections(options);
    check(fake::preparations == 2 && fake::calls.size() == 2, "same backend can reuse prepared baseline");
    options.backend = Backend::Hvm;
    for (const bool allFail : {false, true}) {
      reset(); fake::read = [allFail](unsigned call, std::uint64_t length) {
        auto result = success(length);
        if ((allFail && call > 1) || call == 3) {
          result.ok = false; result.partial = true; result.bytesDone = 2;
          result.data = QByteArray(2, 'A'); result.failureText = QString("window-unavailable");
        }
        return result;
      };
      results = KernelCleanImageBaseline::scanExecutableSections(options);
      check(!results[0].complete && results[0].unreadableBytes == (allFail ? 8U : 4U)
            && results[0].scannedBytes == (allFail ? 0U : 4U) && results[0].differingBytes == 0,
            "unreadable chunks are coverage gaps, not fabricated differences");
      check(results[0].statusText.value.find("window-unavailable") != std::string::npos,
            "coverage status preserves first read failure");
      checkVerdict(results[0]);
    }
    reset(); fake::read = [](unsigned call, std::uint64_t length) {
      auto result = success(length); if (call == 1) { result.ok = false; result.failureText = QString("header-failed"); } return result;
    };
    results = KernelCleanImageBaseline::scanExecutableSections(options);
    check(!results[0].available && !results[0].complete && fake::calls.size() == 1,
          "failed PE identity read cannot proceed to sections"); checkVerdict(results[0]);
    reset(); fake::enumerationFails = true; callbacks = 0;
    results = KernelCleanImageBaseline::scanExecutableSections(options);
    check(callbacks == 1 && results.size() == 1 && results[0].backend == Backend::Hvm && fake::calls.empty(),
          "enumeration failure must reach UI callback with provenance"); checkVerdict(results[0]);
    reset(); fake::emptySections = true;
    results = KernelCleanImageBaseline::scanExecutableSections(options);
    check(!results[0].complete && !results[0].available && fake::calls.size() == 1,
          "no executable sections do not prove clean code"); checkVerdict(results[0]);
    reset(); fake::preparationFails = true;
    results = KernelCleanImageBaseline::scanExecutableSections(options);
    check(!results[0].complete && fake::calls.empty(), "disk/CI failure cannot read sections"); checkVerdict(results[0]);
    reset(); std::atomic_bool cancelled(false); options.cancelFlag = &cancelled;
    fake::read = [&cancelled](unsigned call, std::uint64_t length) {
      if (call == 2) cancelled.store(true);
      return success(length);
    };
    results = KernelCleanImageBaseline::scanExecutableSections(options);
    check(results.size() == 1 && !results[0].complete && results[0].scannedBytes == 4 && fake::calls.size() == 2,
          "cancellation at block boundary leaves incomplete result"); checkVerdict(results[0]);
    options.cancelFlag = nullptr;
    reset(); options.moduleFilter = QString("missing");
    results = KernelCleanImageBaseline::scanExecutableSections(options);
    check(results.empty() && fake::calls.empty(), "module filter applies before any read");
    KernelTextIntegrityTab empty; empty.updateVerdict();
    check(empty.label.style.value.find("success") == std::string::npos, "empty result cannot prove a clean scan");
    options.moduleFilter.clear(); reset();
    fake::read = [](unsigned call, std::uint64_t length) {
      auto result = success(length); if (call > 1) result.data = QByteArray(static_cast<qsizetype>(length), 'B'); return result;
    };
    results = KernelCleanImageBaseline::scanExecutableSections(options);
    check(results[0].complete && results[0].unexplainedRangeCount == 2 && results[0].differingBytes == 8,
          "full differing reads retain actual unexplained ranges");
    KernelTextIntegrityTab changed; changed.m_results = results; changed.updateVerdict();
    check(changed.label.style.value.find("error") != std::string::npos, "unexplained edits are an error verdict");
    changed.m_hvciEvidenceUsable = changed.m_hvciEnforcing = true; changed.updateVerdict();
    check(changed.label.text.value.find("HVCI") != std::string::npos, "enforcing HVCI escalates unexplained edits");
    reset(); fake::dynamicSites = {0x10000U};
    fake::read = [](unsigned call, std::uint64_t length) {
      auto result = success(length); if (call > 1) result.data = QByteArray(static_cast<qsizetype>(length), 'B'); return result;
    };
    results = KernelCleanImageBaseline::scanExecutableSections(options);
    check(results[0].knownRangeCount == 2 && results[0].unexplainedRangeCount == 0,
          "known dynamic relocation classification preserved");
    std::cout << "Kernel integrity production workflow: " << checks << " checks passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
'''


def main():
    compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise RuntimeError("G++ or Clang++ is required")
    baseline = (KERNEL / "KernelCleanImageBaseline.cpp").read_text(encoding="utf-8-sig")
    tab = (KERNEL / "KernelTextIntegrityTab.cpp").read_text(encoding="utf-8-sig")
    helpers = "\n".join(extract(baseline, name) for name in (
        "baselineText", "cachedTrustedImage", "loadedHeaderBytes", "rangeCoveredByDynamicRelocation"))
    methods = "\n".join(extract(baseline, "KernelCleanImageBaseline::" + name)
                        for name in ("readKernelBytes", "scanExecutableSections"))
    production = "namespace {\n" + helpers + "\n}\nnamespace ks::kernel {\n" + methods + "\n}\n"
    production += extract(tab, "KernelTextIntegrityTab::updateVerdict")
    output = ROOT / ".out" / "kernel-integrity-tests" / ("workflow-" + uuid.uuid4().hex)
    output.mkdir(parents=True)
    (output / "qt_shim.h").write_text(QT, encoding="utf-8")
    for name in ("QString", "QByteArray"):
        (output / name).write_text('#include "qt_shim.h"\n', encoding="utf-8")
    cpp, exe = output / "workflow.cpp", output / "workflow.exe"
    cpp.write_text(PRELUDE + production + TESTS, encoding="utf-8")
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-I", str(output), "-I", str(KERNEL), str(cpp), "-o", str(exe)], check=True)
    environment = dict(os.environ)
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
    subprocess.run([str(exe)], check=True, env=environment)


if __name__ == "__main__":
    main()
