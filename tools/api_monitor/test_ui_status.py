"""Compile the production status renderer against Qt and check readiness states."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
qt=root/".deps/Qt/6.9.3/msvc2022_64"
if not qt.exists(): qt=Path("D:/Software/Qt/6.9.3/msvc2022_64")
source=(root/"Ksword5.1/Ksword5.1/MonitorDock/WinAPIDock.cpp").read_text(encoding="utf-8-sig")
method=source[source.index("void WinAPIDock::updateStatusLabel()"):]
header=(root/"Ksword5.1/Ksword5.1/MonitorDock/WinAPIDock.h").read_text(encoding="utf-8-sig")
state=next(line.strip() for line in header.splitlines() if "enum class HookState" in line)
code=r"""
#include <QApplication>
#include <QLabel>
#include <QTableWidget>
#include <QVariant>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <unordered_map>
namespace ks::ui {
 enum class StatusRole { Warning, Info, Error, Idle };
 void ApplyStatusRole(QLabel* label,StatusRole role) { label->setProperty("role",static_cast<int>(role)); }
}
class WinAPIDock {
public:
 STATE_DECL
 QLabel* m_sessionStatusLabel=nullptr;
 QTableWidget* m_eventTable=nullptr;
 std::uint32_t m_currentSessionPid=42;
 std::atomic_bool m_pipeRunning{true},m_pipeConnected{true};
 HookState m_hookState=HookState::Waiting;
 std::mutex m_pendingMutex;
 std::unordered_map<std::uint32_t,std::uint64_t> m_agentDroppedRows;
 std::deque<int> m_pendingRows;
 std::size_t m_pendingDroppedRows=0,m_evictedRows=0;
 QString eventLossSummary();
 void updateStatusLabel();
};
METHOD
int main(int argc,char** argv) {
 QApplication app(argc,argv); QLabel label;QTableWidget table;table.setRowCount(3);
 WinAPIDock dock;dock.m_sessionStatusLabel=&label;dock.m_eventTable=&table;
 dock.updateStatusLabel();if(!label.text().contains(QStringLiteral("正在安装 Hook"))) return 1;
 dock.m_hookState=WinAPIDock::HookState::Active;dock.updateStatusLabel();
 if(!label.text().contains(QStringLiteral("监控中"))) return 2;
 dock.m_hookState=WinAPIDock::HookState::Partial;dock.updateStatusLabel();
 if(!label.text().contains(QStringLiteral("部分 Hook"))) return 3;
 dock.m_hookState=WinAPIDock::HookState::Failed;dock.updateStatusLabel();
 if(label.text().contains(QStringLiteral("监控中"))||label.property("role").toInt()!=static_cast<int>(ks::ui::StatusRole::Error)) return 4;
 dock.m_pipeConnected.store(false);dock.updateStatusLabel();
 if(!label.text().contains(QStringLiteral("等待 Agent"))) return 5;
 dock.m_pipeRunning.store(false);dock.updateStatusLabel();
 if(!label.text().contains(QStringLiteral("空闲"))) return 6;
 dock.m_agentDroppedRows[42]=12;dock.m_pendingDroppedRows=7;dock.m_evictedRows=99;dock.updateStatusLabel();
 if(!label.text().contains(QStringLiteral("Agent 丢失=12，UI 丢失=7，已移出表格=99")))return 7;
 printf("PASS: connecting, installing, active, partial, failure and idle UI states\n");
}
""".replace("STATE_DECL",state).replace("METHOD",method)
with tempfile.TemporaryDirectory(prefix="ksword_status_") as temp:
 folder=Path(temp);cpp=folder/"test.cpp";cpp.write_text(code,encoding="utf-8")
 includes=["/I"+str(qt/"include"/name) for name in ["","QtCore","QtGui","QtWidgets"]]
 libs=[str(qt/"lib"/name) for name in ["Qt6Core.lib","Qt6Gui.lib","Qt6Widgets.lib"]]
 subprocess.run(["cl","/nologo","/EHsc","/permissive-","/std:c++17","/Zc:__cplusplus","/utf-8","/MD",*includes,str(cpp),"/Fe:"+str(folder/"test.exe"),"/Fo:"+str(folder/"test.obj"),"/link",*libs],check=True)
 subprocess.run([str(qt/"bin/windeployqt.exe"),"--release","--compiler-runtime","--no-opengl-sw",str(folder/"test.exe")],check=True,stdout=subprocess.DEVNULL)
 env=os.environ.copy();pathkey=next(k for k in env if k.lower()=="path");env[pathkey]=str(qt/"bin")+os.pathsep+env[pathkey];env["QT_QPA_PLATFORM"]="offscreen";env["QT_QPA_PLATFORM_PLUGIN_PATH"]=str(qt/"plugins/platforms")
 subprocess.run([str(folder/"test.exe")],check=True,env=env,timeout=30)
