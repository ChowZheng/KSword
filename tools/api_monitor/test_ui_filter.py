"""Check production incremental filtering with 12,000 Qt rows and instrument rescan work."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
qt=root/".deps/Qt/6.9.3/msvc2022_64"
if not qt.exists():qt=Path("D:/Software/Qt/6.9.3/msvc2022_64")
folder=root/"Ksword5.1/Ksword5.1/MonitorDock"
header=(folder/"WinAPIDock.h").read_text(encoding="utf-8-sig")
row=header[header.index("    enum class HookState"):header.index("private:",header.index("    enum class HookState"))]
enum=header[header.index("    enum EventColumn"):header.index("    // FakeRuleColumn")]
actions=(folder/"WinAPIDock.Actions.cpp").read_text(encoding="utf-8-sig")
filtermethod=actions[actions.index("void WinAPIDock::applyEventFilter()"):actions.index("void WinAPIDock::clearEventFilter()")]
pipe=(folder/"WinAPIDock.Pipe.cpp").read_text(encoding="utf-8-sig")
append=pipe[pipe.index("void WinAPIDock::appendEventRow("):]
trim=pipe[pipe.index("        const int removeCount"):pipe.index("        m_eventTable->setUpdatesEnabled(updatesEnabled);")]
code=r'''
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <QColor>
#include <cstdio>
namespace ks::ui {enum class StatusRole {Success,Idle};void ApplyStatusRole(QLabel*,StatusRole){}}
namespace KswordTheme {QColor InfoColor(){return Qt::blue;}QColor ErrorColor(){return Qt::red;}}
struct Progress {template<class... A> void set(A...){}} kPro;
class CountingTable:public QTableWidget {public:int hideCalls=0;void setRowHidden(int row,bool hide){++hideCalls;QTableWidget::setRowHidden(row,hide);}};
class WinAPIDock {public:
ENUM_DECL
ROW_DECL
 CountingTable* m_eventTable=nullptr;QLabel* m_eventFilterStatusLabel=nullptr;QLineEdit* m_eventFilterEdit=nullptr;
 QString m_eventFilterKeyword;int m_visibleEventCount=0;size_t m_evictedRows=0;
 uint32_t m_currentSessionPid=0;int m_sessionProgressPid=0;HookState m_hookState=HookState::Waiting;
 static QTableWidgetItem* createReadOnlyItem(const QString& text){return new QTableWidgetItem(text);}
 void applyEventFilter();void appendEventRow(const EventRow&);
 void trimRows(){TRIM_BODY}
};
FILTER_METHOD
APPEND_METHOD
int main(int argc,char** argv){
 QApplication app(argc,argv);CountingTable table;table.setColumnCount(WinAPIDock::EventColumnCount);
 QLabel label;QLineEdit input;WinAPIDock dock;dock.m_eventTable=&table;dock.m_eventFilterStatusLabel=&label;dock.m_eventFilterEdit=&input;
 WinAPIDock::EventRow row;
 for(int i=0;i<12000;++i){row.apiText=i%2?QStringLiteral("other"):QStringLiteral("needle");dock.appendEventRow(row);}
 if(dock.m_visibleEventCount!=12000)return 1;
 table.hideCalls=0;input.setText(QStringLiteral("needle"));dock.applyEventFilter();
 if(table.hideCalls!=12000||dock.m_visibleEventCount!=6000||!table.isRowHidden(1)||table.isRowHidden(0))return 2;
 table.hideCalls=0;
 for(int i=0;i<160;++i){row.apiText=QStringLiteral("NEEDLE");dock.appendEventRow(row);}
 dock.applyEventFilter();
 if(table.hideCalls!=160||dock.m_visibleEventCount!=6160)return 3;
 dock.trimRows();dock.applyEventFilter();
 if(dock.m_visibleEventCount!=6080||dock.m_evictedRows!=160||table.rowCount()!=12000)return 4;
 table.hideCalls=0;dock.applyEventFilter();if(table.hideCalls!=0)return 5;
 input.clear();dock.applyEventFilter();if(dock.m_visibleEventCount!=12000||table.hideCalls!=12000)return 6;
 printf("PASS: 12,000 rows, keyword rescan once, 160 new-row checks, eviction counts and clearing\n");
}
'''.replace("ENUM_DECL",enum).replace("ROW_DECL",row).replace("FILTER_METHOD",filtermethod).replace("APPEND_METHOD",append).replace("TRIM_BODY",trim)
with tempfile.TemporaryDirectory(prefix="ksword_filter_") as temp:
 folder=Path(temp);cpp=folder/"fixture.cpp";cpp.write_text(code,encoding="utf-8")
 includes=["/I"+str(qt/"include"/name) for name in ["","QtCore","QtGui","QtWidgets"]]
 libs=[str(qt/"lib"/name) for name in ["Qt6Core.lib","Qt6Gui.lib","Qt6Widgets.lib"]]
 subprocess.run(["cl","/nologo","/EHsc","/permissive-","/std:c++17","/Zc:__cplusplus","/utf-8","/MD",*includes,str(cpp),"/Fe:"+str(folder/"fixture.exe"),"/Fo:"+str(folder/"fixture.obj"),"/link",*libs],check=True)
 subprocess.run([str(qt/"bin/windeployqt.exe"),"--release","--compiler-runtime","--no-opengl-sw",str(folder/"fixture.exe")],check=True,stdout=subprocess.DEVNULL)
 env=os.environ.copy();key=next(k for k in env if k.lower()=="path");env[key]=str(qt/"bin")+os.pathsep+env[key];env["QT_QPA_PLATFORM"]="offscreen";env["QT_QPA_PLATFORM_PLUGIN_PATH"]=str(qt/"plugins/platforms")
 subprocess.run([str(folder/"fixture.exe")],check=True,env=env,timeout=30)
