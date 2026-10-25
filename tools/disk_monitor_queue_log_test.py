#!/usr/bin/env python3
"""Compile actual disk-monitor value logic without Windows/Qt, then exercise boundaries.

This is not a Windows IOCTL/ETW or GUI test. Extracted production blocks retain
counter reset/clock checks and model eviction code; the fixture supplies only
plain sample values and Qt row-notification substitutes.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'Ksword5.1/Ksword5.1/HardwareDock'


def function(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 1
    at = opening + 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[start:at]


def main():
    queue = (SOURCE / 'DiskMonitorStoragePanel.cpp').read_text()
    logs = (SOURCE / 'DiskMonitorPage.cpp').read_text()
    counter = function(queue, '    bool counter32Delta(')
    begin = queue.index('        if (!baselineKey.isEmpty() &&', queue.index('double averageQueue'))
    end = queue.index('        // 身份完整', begin)
    calculations = queue[begin:end]
    append = function(logs, '        std::uint64_t append(std::vector<Row> rows)')
    clear = function(logs, '        void clear()')
    code = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>
struct Sample {
    std::uint64_t sampleTickMs=0, bytesRead=0, bytesWritten=0;
    std::uint64_t readTime100ns=0, writeTime100ns=0, idleTime100ns=0, queryTime100ns=0;
    std::uint32_t readCount=0, writeCount=0;
    bool performanceAvailable=true;
};
struct Key { bool empty=false; bool isEmpty() const {return empty;} };
struct Map { const Sample* constEnd() const {return nullptr;} };
struct Result {
    double readRate=0, writeRate=0, activePercent=0, responseTimeMs=0;
    double averageQueue=0, readIops=0, writeIops=0;
    bool rateAvailable=false, responseTimeAvailable=false;
};
''' + counter + r'''
Result calculate(Sample sample, const Sample* baselineIterator, bool missingKey=false) {
    Key baselineKey{missingKey}; Map m_baselineByIdentity;
    Result output;
    auto& readRate=output.readRate; auto& writeRate=output.writeRate;
    auto& activePercent=output.activePercent; auto& responseTimeMs=output.responseTimeMs;
    auto& averageQueue=output.averageQueue; auto& readIops=output.readIops;
    auto& writeIops=output.writeIops; auto& rateAvailable=output.rateAvailable;
    auto& responseTimeAvailable=output.responseTimeAvailable;
    int validRateSampleCount=0;
''' + calculations + r'''
    return output;
}
struct BoundedModel {
    using Row=int;
    std::deque<Row> m_rows;
    int rowCount() const {return static_cast<int>(m_rows.size());}
    void beginRemoveRows(int, int first, int last) {assert(first>=0 && last>=first && last<rowCount());}
    void endRemoveRows() {}
    void beginInsertRows(int, int first, int last) {assert(first==rowCount() && last>=first);}
    void endInsertRows() {}
''' + append + clear + r'''
};
int main() {
    Sample previous; previous.sampleTickMs=1000; previous.queryTime100ns=10000000;
    Sample current=previous; current.sampleTickMs=2000; current.queryTime100ns+=10000000;
    current.bytesRead=1000; current.bytesWritten=2000;
    current.readCount=10; current.writeCount=5;
    current.readTime100ns=20000000; current.writeTime100ns=10000000; current.idleTime100ns=2000000;
    auto r=calculate(current, &previous);
    assert(r.rateAvailable && r.responseTimeAvailable);
    assert(r.averageQueue==3 && r.readIops==10 && r.writeIops==5);
    assert(r.responseTimeMs==200 && r.activePercent==80 && r.readRate==1000 && r.writeRate==2000);
    assert(!calculate(current,nullptr).rateAvailable);
    assert(!calculate(current,&previous,true).rateAvailable);
    Sample reset=current; reset.queryTime100ns=previous.queryTime100ns;
    assert(!calculate(reset,&previous).rateAvailable);
    reset=current; reset.sampleTickMs=previous.sampleTickMs;
    assert(!calculate(reset,&previous).rateAvailable);
    reset=current; reset.queryTime100ns+=1000000000;
    assert(!calculate(reset,&previous).rateAvailable);
    Sample idle=previous; idle.sampleTickMs+=1000; idle.queryTime100ns+=10000000; idle.idleTime100ns=10000000;
    r=calculate(idle,&previous);
    assert(r.rateAvailable && !r.responseTimeAvailable && r.averageQueue==0 && r.activePercent==0);
    Sample huge=current; huge.readTime100ns=UINT64_MAX-1; huge.writeTime100ns=UINT64_MAX-1;
    r=calculate(huge,&previous);
    assert(std::isfinite(r.averageQueue) && r.averageQueue>1e12);
    Sample wrap=previous; wrap.readCount=UINT32_MAX-2;
    current.readCount=3;
    r=calculate(current,&wrap);
    assert(r.rateAvailable && r.readIops==6);
    wrap.readCount=1000; current.readCount=10;
    assert(!calculate(current,&wrap).rateAvailable);
    BoundedModel model; std::uint64_t evicted=0;
    for(int batch=0;batch<100;++batch) {
        std::vector<int> rows;
        for(int i=0;i<2048;++i) rows.push_back(batch*2048+i);
        evicted+=model.append(std::move(rows));
        assert(model.rowCount()<=10000);
    }
    assert(model.rowCount()==10000 && evicted==194800);
    assert(model.m_rows.front()==194800 && model.m_rows.back()==204799);
    assert(model.append({})==0);
    model.clear(); assert(model.rowCount()==0); model.clear();
}
'''
    compiler = shutil.which('c++') or shutil.which('g++')
    if not compiler:
        raise SystemExit('C++ compiler unavailable')
    with tempfile.TemporaryDirectory(prefix='ksword-disk-values-') as temp:
        path = Path(temp)
        (path / 'test.cpp').write_text(code)
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)
    print('PASS: production queue arithmetic/reset/wrap and bounded log model retention')


if __name__ == '__main__':
    main()
