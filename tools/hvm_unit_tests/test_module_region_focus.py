"""抽取真实模块区域定位函数，验证刷新屏障、地址范围和排序后的选区。

只替代 Qt 模型/视图接口，不创建窗口、不枚举真实进程。运行：
python tools/hvm_unit_tests/test_module_region_focus.py
--negative-control 会移除生产刷新屏障，并要求回归检测出旧表提前消费。
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid

sys.dont_write_bytecode = True
from test_memory_bookmarks import extract_method


ROOT = Path(__file__).resolve().parents[2]
PRODUCTION = ROOT / "Ksword5.1/Ksword5.1/MemoryDock/MemoryDock.WorkbenchEntry.cpp"
OUTPUT = ROOT / "output"


# 接口替代只保存真实显示行、选择动作和状态文本，定位逻辑全部来自生产函数。
PRELUDE = r'''
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using QString = std::string;
#define QStringLiteral(value) std::string(value)
namespace Qt { constexpr int UserRole = 256; }
namespace ks::i18n {
QString packedSourceText(const QString& text) { return text; }
}
struct QVariant {
    std::uint64_t value = 0;
    std::uint64_t toULongLong() const { return value; }
};
struct Model;
struct QModelIndex {
    Model* model = nullptr; // 指向本轮显示模型。
    int row = -1, column = -1; // 排序后显示索引，不是缓存索引。
    QVariant data(int role) const;
};
struct Model {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> rows; // 真实显示顺序的基址/长度。
    int rowCount() const { return static_cast<int>(rows.size()); }
    QModelIndex index(int row, int column) { return {this, row, column}; }
};
QVariant QModelIndex::data(int role) const {
    if (role != Qt::UserRole || model == nullptr || row < 0 || row >= model->rowCount()) return {};
    const auto& region = model->rows.at(static_cast<std::size_t>(row));
    return {column == 0 ? region.first : region.second};
}
struct QItemSelectionModel {
    enum { ClearAndSelect = 1, Rows = 2 };
    int selected = -1, calls = 0; // 记录真正的行选中，不把 currentIndex 当选区。
    void select(const QModelIndex& index, int) { selected = index.row; ++calls; }
};
struct QAbstractItemView { enum ScrollHint { PositionAtCenter }; };
struct View {
    Model visible; // 外部异步回填可以替换此模型的行。
    QItemSelectionModel selection;
    int current = -1, scrolled = -1, currentCalls = 0;
    Model* model() { return &visible; }
    QItemSelectionModel* selectionModel() { return &selection; }
    void setCurrentIndex(const QModelIndex& index) { current = index.row; ++currentCalls; }
    void scrollTo(const QModelIndex& index, QAbstractItemView::ScrollHint) { scrolled = index.row; }
};
template<class T> struct QPointer {
    T* pointer; // 测试不伪造 QObject 销毁，仅替代存活守卫的成功分支。
    explicit QPointer(T* value) : pointer(value) {}
    explicit operator bool() const { return pointer != nullptr; }
};
struct Label {
    QString text;
    int changes = 0;
    void setText(const QString& value) { text = value; ++changes; }
};
class MemoryDock {
public:
    enum class CoreKind { Regions };
    struct ModuleRegionFocus {
        std::uint64_t address = 0, attachmentGeneration = 0;
    };
    std::optional<ModuleRegionFocus> m_pendingModuleRegionFocus;
    std::atomic<std::uint64_t> m_processAttachmentGeneration{7};
    View* m_regionTable = nullptr;
    Label* m_regionStatusLabel = nullptr;
    bool m_regionRefreshJob = false, m_regionRefreshPendingJob = false;
    bool projectionCurrent = true;
    bool coreProjectionIsCurrent(CoreKind) const { return projectionCurrent; }
    void applyPendingModuleRegionFocus();
};
'''


# 用一次刷新前后的真实显示行变化重现问题，不靠检查源码是否包含某个 if 判断。
TESTS = r'''
static int checks = 0, failures = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
struct Fixture {
    View view;
    Label status;
    MemoryDock dock;
    Fixture() { dock.m_regionTable = &view; dock.m_regionStatusLabel = &status; }
    void request(std::uint64_t address, std::uint64_t generation = 7) {
        dock.m_pendingModuleRegionFocus = MemoryDock::ModuleRegionFocus{address, generation};
    }
    void expectSelection(int row, const char* message) {
        Check(!dock.m_pendingModuleRegionFocus && view.current == row
            && view.selection.selected == row && view.scrolled == row, message);
    }
};
int main() {
    {
        Fixture test;
        test.view.visible.rows = {{0x1000, 0x1000}};
        test.dock.applyPendingModuleRegionFocus();
        Check(test.view.currentCalls == 0 && test.status.changes == 0, "no request leaves UI unchanged");
        test.request(0x1180);
        test.dock.m_regionRefreshJob = true;
        test.dock.applyPendingModuleRegionFocus();
        Check(test.dock.m_pendingModuleRegionFocus.has_value(), "active refresh retains module focus intent");
        Check(test.view.currentCalls == 0 && test.status.changes == 0, "active refresh cannot select stale broad region");
        // 最终区域拆成不同长度且显示顺序反转，旧 base:size 身份无法恢复新选区。
        test.view.visible.rows = {{0x3000, 0x1000}, {0x1100, 0x200}, {0x1000, 0x100}};
        test.dock.m_regionRefreshJob = false;
        test.dock.applyPendingModuleRegionFocus();
        test.expectSelection(1, "final split snapshot selects the containing sorted display row");
        Check(test.status.changes == 0, "successful final focus does not report missing region");
    }
    {
        Fixture test;
        test.view.visible.rows = {{0x1000, 0x1000}};
        test.request(0x1600);
        test.dock.m_regionRefreshPendingJob = true;
        test.dock.applyPendingModuleRegionFocus();
        Check(test.dock.m_pendingModuleRegionFocus.has_value() && test.view.currentCalls == 0,
            "queued refresh retains focus and never consumes the old model");
        test.dock.m_regionRefreshPendingJob = false;
        test.view.visible.rows = {{0x1600, 0x100}, {0x1000, 0x600}};
        test.dock.applyPendingModuleRegionFocus();
        test.expectSelection(0, "queued refresh resolves the new region boundary");
    }
    {
        Fixture test;
        test.request(0x10, 6);
        test.view.visible.rows = {{0, 0x100}};
        test.dock.applyPendingModuleRegionFocus();
        Check(!test.dock.m_pendingModuleRegionFocus && test.view.currentCalls == 0,
            "attachment generation mismatch discards focus without selecting another process");
    }
    {
        Fixture test;
        test.request(0x1000);
        test.dock.projectionCurrent = false;
        test.view.visible.rows = {{0x1000, 0x100}};
        test.dock.applyPendingModuleRegionFocus();
        Check(test.dock.m_pendingModuleRegionFocus.has_value() && test.view.currentCalls == 0,
            "outdated projection cannot consume module focus");
        test.dock.projectionCurrent = true;
        test.dock.m_regionTable = nullptr;
        test.dock.applyPendingModuleRegionFocus();
        Check(test.dock.m_pendingModuleRegionFocus.has_value(), "unmounted region view keeps pending intent");
    }
    for (const std::uint64_t address : {std::uint64_t{0}, std::uint64_t{0xFF}}) {
        Fixture test;
        test.view.visible.rows = {{0x3000, 0x100}, {0, 0x100}, {0x2000, 0x100}};
        test.request(address);
        test.dock.applyPendingModuleRegionFocus();
        test.expectSelection(1, "zero base and last byte are valid addresses in a sorted row");
    }
    {
        Fixture test;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        test.view.visible.rows = {{maximum - 15, 16}};
        test.request(maximum);
        test.dock.applyPendingModuleRegionFocus();
        test.expectSelection(0, "upper address boundary is checked without overflowing base plus length");
    }
    for (const std::uint64_t address : {std::uint64_t{0x100}, std::uint64_t{0x2000}}) {
        Fixture test;
        test.view.visible.rows = {{0, 0x100}, {0x3000, 0x100}};
        test.request(address);
        test.dock.applyPendingModuleRegionFocus();
        Check(!test.dock.m_pendingModuleRegionFocus && test.view.currentCalls == 0,
            "exclusive upper end and gaps are not valid containing rows");
        Check(test.status.changes == 1 && !test.status.text.empty(), "final missing region has an explicit status");
    }
    {
        Fixture test;
        test.request(0x1234);
        test.dock.applyPendingModuleRegionFocus();
        Check(!test.dock.m_pendingModuleRegionFocus && test.status.changes == 1,
            "final empty snapshot clears pending rather than stealing future refresh selection");
    }
    std::cout << "Production module region focus: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
'''


def main() -> None:
    """编译完整生产函数；只清理本次 output/ 下唯一命名的普通文件。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=PRODUCTION)
    parser.add_argument("--compiler", default=os.environ.get("CXX"))
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        parser.error("G++ or Clang++ is required")
    method = extract_method(args.source.read_text(encoding="utf-8-sig"), "MemoryDock::applyPendingModuleRegionFocus")
    if args.negative_control:
        # 删除屏障后必须由上述可观察选区断言抓到回归，不能仅匹配源码结构声称成功。
        method, count = re.subn(
            r"\s*if\s*\(\s*m_regionRefreshJob\s*\|\|\s*m_regionRefreshPendingJob\s*\)\s*\{\s*return;\s*\}",
            "", method, count=1,
        )
        if count != 1:
            parser.error("Production refresh guard was not found for the negative control")
    stem = f"module-region-focus-{uuid.uuid4().hex}"
    OUTPUT.mkdir(parents=True, exist_ok=True)
    cpp = OUTPUT / f"{stem}.cpp"
    binary = OUTPUT / (stem + (".exe" if os.name == "nt" else ""))
    try:
        cpp.write_text(PRELUDE + method + TESTS, encoding="utf-8")
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
            str(cpp), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], check=False)
        if args.negative_control:
            if result.returncode != 1:
                raise RuntimeError("Negative control was not rejected by the regression assertions")
            print("Negative control: missing refresh guard was detected")
        elif result.returncode:
            raise SystemExit(result.returncode)
    finally:
        for artifact in (cpp, binary):
            if artifact.resolve().parent != OUTPUT.resolve():
                raise RuntimeError("Refusing to clean outside repository output/")
            artifact.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
