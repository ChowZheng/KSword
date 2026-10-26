#!/usr/bin/env python3
"""Run the production treemap partitioner against lightweight geometry adapters.

This needs Python 3 and a C++17 compiler, not Qt. It extracts rebuildLayout()
verbatim from NtfsSpaceMapWidget.cpp and supplies only the container/rectangle
operations that function uses. It checks geometry, not QWidget painting, events,
theme behavior, Windows NTFS access, or GUI responsiveness.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ADAPTERS = r"""
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

using qsizetype = std::int64_t;
using qreal = double;
using quint64 = std::uint64_t;

template<class T> struct QVector : std::vector<T>
{
    using std::vector<T>::vector;
    void append(T value) { this->push_back(value); }
    T takeLast() { T value = this->back(); this->pop_back(); return value; }
    bool isEmpty() const { return this->empty(); }
    qsizetype size() const { return static_cast<qsizetype>(std::vector<T>::size()); }
};

struct QRectF
{
    double x = 0, y = 0, w = 0, h = 0;
    QRectF() = default;
    QRectF(double a, double b, double c, double d) : x(a), y(b), w(c), h(d) {}
    QRectF adjusted(double a, double b, double c, double d) const
        { return { x + a, y + b, w + c - a, h + d - b }; }
    bool isEmpty() const { return w <= 0 || h <= 0; }
    double width() const { return w; }
    double height() const { return h; }
    double right() const { return x + w; }
    double bottom() const { return y + h; }
    void setWidth(double value) { w = value; }
    void setHeight(double value) { h = value; }
    void setLeft(double value) { w = x + w - value; x = value; }
    void setTop(double value) { h = y + h - value; y = value; }
};

struct NtfsSpaceMapWidget
{
    struct Item { quint64 reference; };
    struct Tile { QRectF rectangle; qsizetype first, last; };
    struct Data { QVector<Item> items; QVector<long double> prefixWeights; };
    Data data;
    const Data* m_data = &data;
    QVector<Tile> m_tiles;
    int m_selectedTile = -1;
    bool m_hasSelection = false;
    quint64 m_selectedReference = 0;
    int width() const { return 1000; }
    int height() const { return 600; }
    QRectF rect() const { return { 0, 0, 1000, 600 }; }
    void rebuildLayout();
};
"""

TESTS = r"""
int main()
{
    std::mt19937_64 random(8173);
    for (int trial = 0; trial < 100; ++trial)
    {
        NtfsSpaceMapWidget widget;
        widget.data.prefixWeights.append(0);
        const int count = trial == 99 ? 100000 : 1 + random() % 10000;
        for (int i = 0; i < count; ++i)
        {
            widget.data.items.append({ static_cast<quint64>(i) });
            widget.data.prefixWeights.append(widget.data.prefixWeights.back()
                + 1 + random() % 1000000000);
        }
        widget.rebuildLayout();
        assert(widget.m_tiles.size() <= kMaxTiles);
        std::vector<bool> covered(count);
        long double area = 0;
        constexpr long double canvasArea = 992 * 592;
        for (const auto& tile : widget.m_tiles)
        {
            assert(tile.first >= 0 && tile.last <= count && tile.first < tile.last);
            for (auto i = tile.first; i < tile.last; ++i)
            {
                assert(!covered[i]);
                covered[i] = true;
            }
            assert(tile.rectangle.w > 0 && tile.rectangle.h > 0);
            assert(tile.rectangle.x >= 4 - 1e-8 && tile.rectangle.y >= 4 - 1e-8);
            assert(tile.rectangle.right() <= 996 + 1e-8);
            assert(tile.rectangle.bottom() <= 596 + 1e-8);
            const long double tileArea = tile.rectangle.w * tile.rectangle.h;
            area += tileArea;
            const long double share = (widget.data.prefixWeights[tile.last]
                - widget.data.prefixWeights[tile.first]) / widget.data.prefixWeights.back();
            assert(std::abs(tileArea / canvasArea - share) < 1e-10L);
        }
        assert(std::all_of(covered.begin(), covered.end(), [](bool value) { return value; }));
        assert(std::abs(area - canvasArea) < 1e-5L);
    }
    std::cout << "100 production rebuildLayout trials passed (up to 100000 items): "
        "coverage, area ratios, canvas bounds, tile budget. Geometry adapters only; no QWidget GUI.\n";
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++", help="C++17 compiler executable")
    args = parser.parse_args()
    compiler = shutil.which(args.compiler)
    if not compiler:
        raise SystemExit(f"C++17 compiler not found: {args.compiler}")
    root = Path(__file__).resolve().parents[1]
    source = (root / "Ksword5.1/Ksword5.1/FileDock/NtfsSpaceMapWidget.cpp").read_text(encoding="utf-8")
    start = source.index("void NtfsSpaceMapWidget::rebuildLayout()")
    end = source.index("\nvoid NtfsSpaceMapWidget::paintEvent", start)
    budget = re.search(r"constexpr qsizetype kMaxTiles = (\d+);", source)
    if not budget:
        raise SystemExit("Production tile budget declaration not found")
    production = source[start:end]
    with tempfile.TemporaryDirectory(prefix="ksword-treemap-test-") as directory:
        build = Path(directory)
        fixture = build / "layout.cpp"
        executable = build / "layout-test"
        fixture.write_text(ADAPTERS + "\n" + budget.group(0) + "\n" + production + TESTS,
                           encoding="utf-8")
        subprocess.run([compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(fixture), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
