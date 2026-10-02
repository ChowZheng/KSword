"""Compile real Qt coverage rendering/queue methods and verify stale snapshot isolation."""
from pathlib import Path
import subprocess,tempfile,os,hashlib
root=Path(__file__).resolve().parents[2]
qt=root/'.deps/Qt/6.9.3/msvc2022_64'
if not qt.exists():qt=Path('D:/Software/Qt/6.9.3/msvc2022_64')
source=(root/'Ksword5.1/Ksword5.1/MonitorDock/WinAPIDock.Coverage.cpp').read_text(encoding='utf-8-sig')
source=source[source.index('namespace\n'):]
code=r'''
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTabWidget>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include "PROTOCOL"
namespace ks::ui {enum class StatusRole {Warning,Info};void ApplyStatusRole(QLabel* label,StatusRole role){label->setProperty("role",static_cast<int>(role));}}
class WinAPIDock {public:
 struct CoverageViewState {uint64_t session=0,revision=0;bool stale=true,complete=false;std::vector<ks::winapi_monitor::ApiMonitorEventPacket> rows;};
 std::mutex m_pendingMutex;std::atomic_uint64_t m_sessionGeneration{1};
 std::unordered_map<uint32_t,CoverageViewState> m_pendingCoverage,m_coverageViews;
 QComboBox* m_coveragePidCombo=nullptr;QLineEdit* m_coverageFilterEdit=nullptr;QLabel* m_coverageStatusLabel=nullptr;
 QTableWidget* m_coverageTable=nullptr;QTabWidget* m_resultTabs=nullptr;bool m_coverageDirty=true,m_definitionMismatch=false;uint32_t m_currentSessionPid=42;
 static QTableWidgetItem* createReadOnlyItem(const QString& text){return new QTableWidgetItem(text);}
 void queueCoverageSnapshot(uint32_t,uint64_t,uint64_t,std::vector<ks::winapi_monitor::ApiMonitorEventPacket>,bool,uint64_t);
 void flushCoverageUpdates();void updateCoverageView();void resetCoverageView();void markCoverageStale();
};
SOURCE
#define CHECK(x) do {if(!(x)){printf("FAIL line %d\n",__LINE__);return 1;}}while(0)
int main(int argc,char** argv){
 QApplication app(argc,argv);QComboBox combo;QLineEdit filter;QLabel label;QTableWidget table;table.setColumnCount(7);QTabWidget tabs;
 tabs.addTab(new QWidget,&quot;events&quot;);tabs.addTab(new QWidget,&quot;coverage&quot;);tabs.setCurrentIndex(1);
 WinAPIDock dock;dock.m_coveragePidCombo=&combo;dock.m_coverageFilterEdit=&filter;dock.m_coverageStatusLabel=&label;dock.m_coverageTable=&table;dock.m_resultTabs=&tabs;
 using namespace ks::winapi_monitor;std::vector<ApiMonitorEventPacket> rows(2);
 for(int i=0;i<2;++i){rows[i].apiId=i+1;rows[i].coverageState=i;wcscpy_s(rows[i].moduleName,L"KernelBase");wcscpy_s(rows[i].apiName,i?L"WriteFile":L"ReadFile");memcpy(rows[i].definitionSha256,"HASH",65);}
 dock.queueCoverageSnapshot(42,123,1,rows,false,0);dock.flushCoverageUpdates();CHECK(dock.m_coverageViews.empty());
 dock.queueCoverageSnapshot(42,123,2,rows,false,1);dock.flushCoverageUpdates();
 CHECK(table.rowCount()==2&&!dock.m_definitionMismatch&&label.text().contains(QStringLiteral("完整")));
 dock.queueCoverageSnapshot(42,123,1,{},false,1);dock.flushCoverageUpdates();CHECK(table.rowCount()==2&&dock.m_coverageViews[42].revision==2);
 dock.queueCoverageSnapshot(42,456,99,{},false,1);dock.flushCoverageUpdates();CHECK(dock.m_coverageViews[42].session==123);
 filter.setText(QStringLiteral("WriteFile"));dock.updateCoverageView();CHECK(table.rowCount()==1&&table.item(0,4)->text()==QStringLiteral("共享入口"));
 filter.clear();rows.resize(1);rows[0].coverageState=static_cast<unsigned>(CoverageState::RetryableFailure);
 dock.queueCoverageSnapshot(42,123,3,rows,false,1);dock.queueCoverageSnapshot(42,123,3,{},true,1);dock.flushCoverageUpdates();
 CHECK(table.rowCount()==1&&dock.m_coverageViews[42].revision==3&&label.text().contains(QStringLiteral("过期")));
 rows[0].definitionSha256[0]='x';dock.queueCoverageSnapshot(42,123,4,rows,false,1);dock.flushCoverageUpdates();CHECK(dock.m_definitionMismatch);
 ++dock.m_sessionGeneration;dock.resetCoverageView();dock.queueCoverageSnapshot(42,123,100,rows,false,1);dock.flushCoverageUpdates();CHECK(dock.m_coverageViews.empty());
 printf("PASS: Qt coverage rendering, PID selection, filters, coalesced disconnect, hashes and generation/session/revision isolation\n");
}
'''.replace('&quot;','"').replace('SOURCE',source).replace('PROTOCOL',(root/'shared/WinApiMonitorProtocol.h').as_posix()).replace('HASH',hashlib.sha256((root/'APIMonitor_x64/api_monitor_definitions.json').read_bytes()).hexdigest())
with tempfile.TemporaryDirectory(prefix='ksword_coverage_ui_') as temporary:
 folder=Path(temporary);cpp=folder/'fixture.cpp';cpp.write_text(code,encoding='utf-8');(folder/'profiles').mkdir();(folder/'profiles/api_monitor_definitions.json').write_bytes((root/'APIMonitor_x64/api_monitor_definitions.json').read_bytes())
 includes=['/I'+str(qt/'include'/name) for name in ['', 'QtCore','QtGui','QtWidgets']]
 libs=[str(qt/'lib'/name) for name in ['Qt6Core.lib','Qt6Gui.lib','Qt6Widgets.lib']]
 subprocess.run(['cl','/nologo','/EHsc','/permissive-','/std:c++17','/Zc:__cplusplus','/utf-8','/MD',*includes,str(cpp),'/Fe:'+str(folder/'fixture.exe'),'/Fo:'+str(folder/'fixture.obj'),'/link',*libs],check=True)
 subprocess.run([str(qt/'bin/windeployqt.exe'),'--release','--compiler-runtime','--no-opengl-sw',str(folder/'fixture.exe')],check=True,stdout=subprocess.DEVNULL)
 env=os.environ.copy();pathkey=next(k for k in env if k.lower()=='path');env[pathkey]=str(qt/'bin')+os.pathsep+env[pathkey];env['QT_QPA_PLATFORM']='offscreen';env['QT_QPA_PLATFORM_PLUGIN_PATH']=str(qt/'plugins/platforms')
 subprocess.run([str(folder/'fixture.exe')],check=True,env=env,timeout=30)
