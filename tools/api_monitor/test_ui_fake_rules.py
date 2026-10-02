"""Round-trip the real Qt Fake rule serializer/validator through the Agent parser."""
from pathlib import Path
import os
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
folder=root/"Ksword5.1/Ksword5.1/MonitorDock"
header=(folder/"WinAPIDock.h").read_text(encoding="utf-8-sig")
columns=header[header.index("    enum FakeRuleColumn"):header.index("    // EventRow")]
actions=(folder/"WinAPIDock.Actions.cpp").read_text(encoding="utf-8-sig")
helpers=actions[actions.index("    QString tableCellText("):actions.index("    QString normalizeFakeIntegerText(")]
methods=actions[actions.index("QString WinAPIDock::fakeSuccessRulesIniText()"):actions.index("void WinAPIDock::addFakeSuccessRuleFromInputs()")]
code=r'''
#include "CONFIG_CPP"
#include <QApplication>
#include <QTableWidget>
#include <QSet>
#include <cstdio>
HELPERS
class WinAPIDock {public:
COLUMNS
 QTableWidget* m_fakeRuleTable=nullptr;
 QString fakeSuccessRulesIniText() const;bool validateFakeSuccessRules(QString*) const;
};
METHODS
int main(int argc,char** argv){
 QApplication app(argc,argv);QTableWidget table;WinAPIDock dock;dock.m_fakeRuleTable=&table;
 table.setColumnCount(WinAPIDock::FakeRuleColumnCount);table.setRowCount(1);
 QStringList fields={"custom.dll","Fixture","scalar","-1","win32","5",""};
 for(int i=0;i<fields.size();++i)table.setItem(0,i,new QTableWidgetItem(fields[i]));
 QString error;
 for(const auto& value:QStringList{"","0","8","65532"}){
  table.item(0,WinAPIDock::FakeRuleColumnX86StackBytes)->setText(value);
  if(!dock.validateFakeSuccessRules(&error))return 1;
  auto rules=apimon::ParseFakeSuccessRules(dock.fakeSuccessRulesIniText().toStdWString());
  if(rules.size()!=1||rules[0].returnValue!=UINT64_MAX||rules[0].lastErrorValue!=5)return 2;
  if(value.isEmpty()){if(rules[0].x86StackBytes)return 3;}
  else if(!rules[0].x86StackBytes||*rules[0].x86StackBytes!=value.toUInt())return 4;
 }
 for(const auto& value:QStringList{"1","65536","-1","8;custom","8|0","abcd"}){
  table.item(0,WinAPIDock::FakeRuleColumnX86StackBytes)->setText(value);
  if(dock.validateFakeSuccessRules(&error)||error.isEmpty())return 5;
 }
 auto legacy=apimon::ParseFakeSuccessRules(L"custom.dll|Fixture|scalar|1|none|0");
 if(legacy.size()!=1||legacy[0].x86StackBytes)return 6;
 table.item(0,WinAPIDock::FakeRuleColumnX86StackBytes)->setText("8");
 table.item(0,WinAPIDock::FakeRuleColumnLastErrorKind)->setText(QStringLiteral("不修改 LastError"));
 table.item(0,WinAPIDock::FakeRuleColumnLastErrorKind)->setData(Qt::UserRole,"none");
 auto serialized=dock.fakeSuccessRulesIniText();
 if(!serialized.contains("|none|5|8"))return 7;
 printf("PASS: real Qt seven-field rules and Agent parser, automatic/explicit ABI, numeric limits, delimiters, localized tokens and six-field compatibility\n");
}
'''.replace("CONFIG_CPP",(root/"APIMonitor_x64/core/MonitorConfig.cpp").as_posix()).replace("COLUMNS",columns).replace("HELPERS",helpers).replace("METHODS",methods)
qt=root/".deps/Qt/6.9.3/msvc2022_64"
if not qt.exists():qt=Path("D:/Software/Qt/6.9.3/msvc2022_64")
with tempfile.TemporaryDirectory(prefix="ksword_fake_ui_") as temporary:
    directory=Path(temporary);cpp=directory/"fixture.cpp";cpp.write_text(code,encoding="utf-8")
    includes=["/I"+str(qt/"include"/name) for name in ("","QtCore","QtGui","QtWidgets")]
    libraries=[str(qt/"lib"/name) for name in ("Qt6Core.lib","Qt6Gui.lib","Qt6Widgets.lib")]
    executable=directory/"fixture.exe"
    subprocess.run(["cl","/nologo","/EHsc","/permissive-","/std:c++17","/Zc:__cplusplus","/utf-8","/MD","/I"+str(root/"APIMonitor_x64"),*includes,
                    str(cpp),"/Fe:"+str(executable),"/Fo:"+str(directory/"fixture.obj"),"/link",*libraries],check=True)
    subprocess.run([str(qt/"bin/windeployqt.exe"),"--release","--compiler-runtime","--no-opengl-sw",str(executable)],
                   check=True,stdout=subprocess.DEVNULL)
    environment=os.environ.copy();environment["QT_QPA_PLATFORM"]="offscreen"
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"]=str(qt/"plugins/platforms")
    subprocess.run([str(executable)],env=environment,check=True,timeout=30)
