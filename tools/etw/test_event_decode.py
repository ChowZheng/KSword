"""Compile the production ETW decoder with x64 MSVC and run focused regressions."""
from pathlib import Path
import tempfile,subprocess,os
root=Path(__file__).resolve().parents[2];s=(root/'Ksword5.1/Ksword5.1/MonitorDock/MonitorDock.cpp').read_text(encoding='utf-8-sig');h=(root/'Ksword5.1/Ksword5.1/MonitorDock/MonitorDock.h').read_text(encoding='utf-8-sig')
qt=Path(os.environ.get('KSWORD_QT_DIR',root/'.deps/Qt/6.9.3/msvc2022_64'))
if not qt.exists():qt=Path('D:/Software/Qt/6.9.3/msvc2022_64')
vsdev=next((p for p in [Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/Tools/VsDevCmd.bat'),Path('D:/Software/VS/Common7/Tools/VsDevCmd.bat')] if p.exists()),None)
if vsdev is None:raise SystemExit('x64 MSVC build environment is required')
import argparse, struct
parser=argparse.ArgumentParser()
parser.add_argument('--case', default='all')
args=parser.parse_args()
payload=struct.pack('<QQIIIBBHQIIII',0xfffff8024ffb0000,0x16000,0,92554,2243701516,12,5,0,0,0,0,0,0)
payload+=('\\SystemRoot\\System32\\drivers\\ndiscap.sys'+'\0').encode('utf-16le')
def fun(sig):
 a=s.index('    '+sig);b=s.index('\n    }',a)+len('\n    }');return s[a:b]
code=r'''
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <sddl.h>
#include <objbase.h>
#include <QString>
#include <QStringList>
#include <QRegularExpression>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonParseError>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <limits>
#include <cstdint>
#include <cstring>
#include <cstdio>
class QWidget;class QCheckBox;class QPushButton;class QLabel;class QLineEdit;class QTimer;class QComboBox;
class MonitorDock{public: BLOCKS };
DEPENDENCIES
PARSER
EtwSchemaPropertyEntry prop(ULONG i,const char* name,USHORT type,USHORT count=1){EtwSchemaPropertyEntry p;p.propertyIndex=i;p.propertyNameText=QString::fromLatin1(name);p.normalizedNameText=normalizeEtwPropertyName(p.propertyNameText);p.inType=type;p.fixedCount=count;return p;}
int main(){
 std::vector<EtwDecodedPropertyEntry> decoded;ULONG parsed=0;QString tail;EtwSchemaEntry schema;EVENT_RECORD record{};
 record.EventHeader.Flags=EVENT_HEADER_FLAG_64_BIT_HEADER;record.EventHeader.ProcessId=3212;record.EventHeader.ThreadId=62272;
 auto run=[&](EtwSchemaEntry in,void* raw,USHORT len){record.UserData=raw;record.UserDataLength=len;decodeEtwPropertiesBySchema(&record,in,&decoded,&parsed,&tail);};
 auto fill=[&](QString provider,QString event,QString opcode){MonitorDock::EtwCapturedEventRow row;row.headerPid=record.EventHeader.ProcessId;row.headerTid=record.EventHeader.ThreadId;row.providerGuid=guidToText(record.EventHeader.ProviderId);row.opcode=record.EventHeader.EventDescriptor.Opcode;auto semantic=inferEtwSemanticSummary(provider,event,opcode,decoded);fillEtwCapturedRowDecodedFields(&row,provider,event,semantic,decoded);return row;};
 unsigned char image[]={SAMPLE_BYTES};GUID imageGuid={0x2cb15d1d,0x5fc1,0x11d2,{0xab,0xe1,0x00,0xa0,0xc9,0x11,0xf5,0x18}};
 record.EventHeader.ProviderId=imageGuid;record.EventHeader.Flags|=EVENT_HEADER_FLAG_CLASSIC_HEADER;record.EventHeader.EventDescriptor.Version=3;record.EventHeader.EventDescriptor.Opcode=3;record.UserData=image;record.UserDataLength=sizeof(image);
 EtwSchemaEntry installed;if(!tryBuildEtwSchemaByTdh(&record,&installed)){printf("FAIL installed ImageV3 TDH schema unavailable\n");return 99;}
 run(installed,image,sizeof(image));auto row=fill(guidToText(imageGuid),QStringLiteral("Image"),QStringLiteral("DCStart"));
 TEST_BODY
 printf("PASS: CASE_NAME\n");
}
'''
code=code.replace('BLOCKS',h[h.index('    enum class EtwFilterStage'):h.index('\npublic:',h.index('    struct EtwCapturedEventRow'))])
aliases='using EtwFilterFieldId=MonitorDock::EtwFilterFieldId;using EtwFilterFieldType=MonitorDock::EtwFilterFieldType;using EtwStringMatchMode=MonitorDock::EtwStringMatchMode;using EtwFilterStage=MonitorDock::EtwFilterStage;'
descriptor=s[s.index('    struct EtwFilterFieldDescriptor'):s.index('    constexpr const char* kEtwFilterConfigRelativePath')]
identity='\n'.join(fun(x) for x in ['std::uint32_t etwRelatedProcessId(', 'std::uint32_t etwRelatedThreadId(', 'void etwUpdateRelatedIdentity('])
filters=s[s.index('    const std::vector<EtwFilterFieldDescriptor>& etwFilterFieldDescriptorList()'):s.index('    // EtwSchemaPropertyEntry')]
code=code.replace('DEPENDENCIES',fun('QString guidToText(')+identity+aliases+descriptor+filters)
code=code.replace('PARSER',s[s.index('    // EtwSchemaPropertyEntry'):s.index('    // 100ns 时间戳文本格式化')]).replace('SAMPLE_BYTES',','.join(str(x) for x in payload))
tests={
 'identity': r'''
 if(!row.targetPidValid||row.targetPid!=0||row.headerPid!=3212||parsed!=138){printf("identity failure valid=%d pid=%u header=%u parsed=%u size=%zu\n",row.targetPidValid,row.targetPid,row.headerPid,parsed,sizeof(image));return 1;}
 if(row.pidTidText!=QStringLiteral("0 / 未知")||etwRelatedProcessId(row)!=0)return 2;
 MonitorDock::EtwSimpleFilterCompiled pid;pid.pidRangeList.push_back({3212,3212});
 if(etwSimplePidMatches(pid,row))return 3;
 pid.pidRangeList={{0,0}};if(!etwSimplePidMatches(pid,row))return 4;
 bool needsDecode=false;MonitorDock::EtwCapturedEventRow headerOnly;headerOnly.headerPid=3212;
 pid.pidRangeList={{3212,3212}};
 if(!etwSimpleFilterMatchesHeaderFields(pid,headerOnly,&needsDecode)||!needsDecode)return 5;
 record.EventHeader.EventDescriptor.Opcode=10;
 std::uint32_t child[]={2222,3212};schema.propertyList={prop(0,"NewProcessId",TDH_INTYPE_UINT32),prop(1,"ProcessId",TDH_INTYPE_UINT32)};
 run(schema,child,sizeof(child));auto created=fill(QStringLiteral("Security"),QStringLiteral("4688"),QString());
 if(etwRelatedProcessId(created)!=2222||created.headerPid!=3212)return 6;
 auto json=QJsonDocument::fromJson(buildEtwDetailJson(&record,QString(),QString(),schema,EtwSemanticSummary{},decoded,parsed,tail).toUtf8()).object();
 if(json.value(QStringLiteral("meta")).toObject().value(QStringLiteral("header_pid")).toInt()!=3212)return 7;
 ''',
}
selected=list(tests) if args.case=='all' else [args.case]
if any(name not in tests for name in selected):parser.error('Unknown case')
body='\n'.join('{'+tests[name]+'}' for name in selected)
code=code.replace('TEST_BODY',body).replace('CASE_NAME',','.join(selected))

with tempfile.TemporaryDirectory(prefix='ksword_etw_decode_') as temp:
 d=Path(temp);cpp=d/'audit.cpp';cpp.write_text(code,encoding='utf-8')
 args=['cl','/nologo','/permissive-','/EHsc','/std:c++17','/Zc:__cplusplus','/utf-8','/MD','/I'+str(qt/'include'),'/I'+str(qt/'include/QtCore'),str(cpp),'/Fe:'+str(d/'audit.exe'),'/Fo:'+str(d/'audit.obj'),'/link',str(qt/'lib/Qt6Core.lib'),'tdh.lib','advapi32.lib','ole32.lib']
 bat=d/'build.cmd';bat.write_text('@echo off\ncall "'+str(vsdev)+'" -arch=x64 -host_arch=x64 >nul\n'+subprocess.list2cmdline(args)+'\n',encoding='utf-8')
 r=subprocess.run(['cmd','/d','/c',str(bat)],cwd=temp,capture_output=True,text=True,encoding='utf-8',errors='replace');print(r.stdout);print(r.stderr)
 if r.returncode:raise SystemExit(r.returncode)
 subprocess.run([str(qt/'bin/windeployqt.exe'),'--release','--compiler-runtime','--no-opengl-sw',str(d/'audit.exe')],check=True,stdout=subprocess.DEVNULL)
 subprocess.run([str(d/'audit.exe')],check=True,timeout=30)
