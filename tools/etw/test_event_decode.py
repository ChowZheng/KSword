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
parser.add_argument('--catalog-output', type=Path, help='Audit installed preset manifests and save their raw names and current interpretation')
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
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QFile>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
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
 auto run=[&](EtwSchemaEntry in,void* raw,USHORT len){record.UserData=raw;record.UserDataLength=len;return decodeEtwPropertiesBySchema(&record,in,&decoded,&parsed,&tail);};
 auto fill=[&](QString provider,QString event,QString opcode){MonitorDock::EtwCapturedEventRow row;row.headerPid=record.EventHeader.ProcessId;row.headerTid=record.EventHeader.ThreadId;row.providerGuid=guidToText(record.EventHeader.ProviderId);row.opcode=record.EventHeader.EventDescriptor.Opcode;auto semantic=inferEtwSemanticSummary(provider,event,opcode,decoded,record.EventHeader.EventDescriptor.Opcode);fillEtwCapturedRowDecodedFields(&row,provider,event,semantic,decoded);return row;};
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
identity='\n'.join(fun(x) for x in ['QString etwProviderDisplayName(', 'std::uint32_t etwRelatedProcessId(', 'std::uint32_t etwRelatedThreadId(', 'void etwUpdateRelatedIdentity('])
filters=s[s.index('    const std::vector<EtwFilterFieldDescriptor>& etwFilterFieldDescriptorList()'):s.index('    // EtwSchemaPropertyEntry')]
code=code.replace('DEPENDENCIES',fun('QString guidToText(')+identity+aliases+descriptor+filters)
presets=s[s.index('    struct EtwPresetProviderDescriptor'):s.index('    constexpr GUID kKswordEtwKernelSessionGuid')]
presets+='\n'+fun('const std::vector<EtwPresetProviderDescriptor>& etwPresetProviderDescriptorList(')+'\n'+fun('const EtwPresetProviderDescriptor* findEtwPresetProviderDescriptor(')+'\n'+fun('QString etwInferProviderCategory(')
code=code.replace('PARSER',presets+'\nPARSER')
code=code.replace('PARSER',s[s.index('    // EtwSchemaPropertyEntry'):s.index('    // 100ns 时间戳文本格式化')]).replace('SAMPLE_BYTES',','.join(str(x) for x in payload))
tests={
 'file_translation': r'''
 if(inferEtwActionText(QStringLiteral("FileIo"),QStringLiteral("Create"),QStringLiteral("Kernel-FileIO"),64)!=QStringLiteral("创建/打开"))return 160;
 if(inferEtwActionText(QStringLiteral("NameDelete"),QString(),QStringLiteral("Microsoft-Windows-Kernel-File"),0)!=QStringLiteral("移除文件名记录"))return 161;
 if(inferEtwActionText(QStringLiteral("SetDelete"),QString(),QStringLiteral("Microsoft-Windows-Kernel-File"),0)!=QStringLiteral("设置删除标记"))return 162;
 EtwDecodedPropertyEntry object;object.normalizedNameText=QStringLiteral("fileobject");object.valueText=QStringLiteral("0xffff800000001000");decoded={object};
 auto semantic=inferEtwSemanticSummary(QStringLiteral("Kernel-FileIO"),QStringLiteral("FileIo"),QStringLiteral("Read"),decoded,67);
 if(!semantic.targetText.isEmpty())return 163;
 EtwDecodedPropertyEntry path;path.normalizedNameText=QStringLiteral("openpath");path.valueText=QStringLiteral("C:\\existing.txt");decoded.push_back(path);
 record.EventHeader.EventDescriptor.Opcode=64;auto opened=fill(QStringLiteral("Kernel-FileIO"),QStringLiteral("FileIo"),QStringLiteral("Create"));
 if(opened.filePathText!=path.valueText||opened.targetText!=path.valueText||opened.actionText!=QStringLiteral("创建/打开"))return 164;
 ''',
 'opcode_translation': r'''
 struct OpcodeCase{const char* provider;const char* event;const char* opcode;int value;const char* expected;};
 const OpcodeCase cases[]={
 {"Kernel-TCPIP","TcpIp","SendIPV4",10,"发送"},{"Kernel-UDPIP","UdpIp","RecvIPV6",27,"接收"},
 {"Kernel-TCPIP","TcpIp","DisconnectIPV4",13,"断开连接"},
 {"Kernel-FileIO","FileIo","QueryInfo",74,"读取/查询"},{"Kernel-FileIO","FileIo","SetInfo",69,"写入/设置"},
 {"Kernel-FileIO","FileIo","OperationEnd",76,"完成"},
 {"Microsoft-Windows-Kernel-Image","Image","LoaderError",164,"LoaderError"},
 {"Microsoft-Windows-Kernel-Thread","Thread","Thread Migration",61,"Thread Migration"},
 {"Kernel-PageFault","PageFault","CopyOnWrite",12,"CopyOnWrite"},
 {"Microsoft-Windows-TCPIP","TcpConnectTcbTimeout","信息",0,"TcpConnectTcbTimeout"}};
 for(const auto& c:cases)if(inferEtwActionText(QString::fromUtf8(c.event),QString::fromUtf8(c.opcode),QString::fromUtf8(c.provider),c.value)!=QString::fromUtf8(c.expected))return 150;
 ''',
 'classic_categories': r'''
 struct ClassicCase{const char* guid;int opcode;const char* provider;const char* resource;};
 const ClassicCase cases[]={
 {"{3D6FA8D1-FE05-11D0-9DDA-00C04FD7BA7C}",36,"Kernel-CSwitch","性能分析"},
 {"{3D6FA8D4-FE05-11D0-9DDA-00C04FD7BA7C}",10,"Kernel-DiskIO","磁盘"},
 {"{3D6FA8D4-FE05-11D0-9DDA-00C04FD7BA7C}",34,"Kernel-Driver","驱动"},
 {"{3D6FA8D3-FE05-11D0-9DDA-00C04FD7BA7C}",32,"Kernel-HardFault","内存"},
 {"{3D6FA8D3-FE05-11D0-9DDA-00C04FD7BA7C}",98,"Kernel-VirtualAlloc","内存"},
 {"{CE1DBFB4-137E-4DA6-87B0-3F59AA102CBC}",46,"Kernel-Profile","性能分析"},
 {"{CE1DBFB4-137E-4DA6-87B0-3F59AA102CBC}",68,"Kernel-DPC","性能分析"},
 {"{CE1DBFB4-137E-4DA6-87B0-3F59AA102CBC}",51,"Kernel-SystemCall","性能分析"},
 {"{45D8CCCD-539F-4B72-A8B7-5C683142609A}",33,"Kernel-ALPC","进程间通信"}};
 for(const auto& c:cases){auto guid=QString::fromLatin1(c.guid);auto name=etwProviderDisplayName(guid,guid,c.opcode);if(name!=QString::fromLatin1(c.provider)||inferEtwResourceType(name,QString())!=QString::fromUtf8(c.resource))return 140;}
 ''',
 'status_translation': r'''
 schema={};schema.propertyList={prop(0,"Result",TDH_INTYPE_BOOLEAN)};std::uint32_t result=0;
 run(schema,&result,sizeof(result));auto semantic=inferEtwSemanticSummary(QStringLiteral("Microsoft-Windows-Security-Auditing"),QStringLiteral("Audit"),QString(),decoded);
 if(semantic.statusText!=decoded[0].valueText||semantic.statusText==QStringLiteral("成功"))return 130;
 schema.propertyList={prop(0,"Status",TDH_INTYPE_UINT32)};result=1;
 run(schema,&result,sizeof(result));semantic=inferEtwSemanticSummary(QStringLiteral("Vendor"),QStringLiteral("State"),QString(),decoded);
 if(semantic.statusText!=QStringLiteral("1"))return 131;
 schema.propertyList={prop(0,"Status",TDH_INTYPE_INT32)};schema.propertyList[0].outType=TDH_OUTTYPE_NTSTATUS;result=0xc0000005;
 run(schema,&result,sizeof(result));semantic=inferEtwSemanticSummary(QStringLiteral("Kernel-FileIO"),QStringLiteral("OperationEnd"),QString(),decoded);
 if(semantic.statusText!=decoded[0].valueText||semantic.statusText.contains(QStringLiteral("FFFFFFFF")))return 132;
 if(etwPropertyMeaningText(QStringLiteral("result"))!=QStringLiteral("结果"))return 133;
 ''',
 'action_translation': r'''
 struct ActionCase{const char* provider;const char* event;const char* opcode;int value;const char* expected;};
 const ActionCase cases[]={
 {"Microsoft-Windows-Kernel-Process","ProcessStart","开始",1,"创建/启动"},
 {"Microsoft-Windows-Kernel-Process","ProcessStop","停止",2,"结束"},
 {"Microsoft-Windows-Kernel-Process","ProcessFreeze","停止",2,"结束"},
 {"Microsoft-Windows-Kernel-Registry","HiveMount","开始",1,"开始"},
 {"Microsoft-Windows-Kernel-Registry","EnumerateValueKey","EnumerateValueKey",40,"枚举"},
 {"Kernel-FileIO","FileIoRead","Read",67,"读取/查询"},
 {"Vendor-Custom","Offset","",0,"Offset"},{"Vendor-Custom","Setup","",0,"Setup"},
 {"Vendor-Custom","Reset","",0,"重置"},
 {"Microsoft-Windows-TCPIP","TcpFastopenStateChange","信息",0,"TcpFastopenStateChange"},
 {"Microsoft-Windows-TCPIP","TcpConnectTcbTimeout","信息",0,"TcpConnectTcbTimeout"},
 {"Microsoft-Windows-TCPIP","TcpCreateEndpointAfFailure","信息",0,"TcpCreateEndpointAfFailure"},
 {"Microsoft-Windows-Kernel-Image","Image","卸载",2,"卸载"}};
 for(const auto& c:cases){auto got=inferEtwActionText(QString::fromUtf8(c.event),QString::fromUtf8(c.opcode),QString::fromUtf8(c.provider),c.value);if(got!=QString::fromUtf8(c.expected)){printf("action mismatch: %s got=%s\n",c.event,qPrintable(got));return 120;}}
 ''',
 'provider_categories': r'''
 for(const auto& preset:etwPresetProviderDescriptorList())if(etwInferProviderCategory(preset.providerNameText)!=preset.categoryText)return 110;
 struct ResourceCase{const char* provider;const char* event;const char* resource;};
 const ResourceCase cases[]={
 {"Microsoft-Windows-TCPIP","Ndkpi_Deregister_Mr","网络"},{"Microsoft-Windows-DNS-Client","DnsRegistration","网络"},
 {"Microsoft-Windows-Winsock-AFD","Deregister","网络"},{"Kernel-Profile","Profile","性能分析"},
 {"Microsoft-Windows-Security-Auditing","Audit","安全审计"},{"Microsoft-Windows-Windows Defender","Detection","安全审计"},
 {"Microsoft-Windows-PowerShell","ScriptBlock","脚本管理"},{"Microsoft-Windows-WMI-Activity","Operation","脚本管理"},
 {"Microsoft-Windows-TaskScheduler","Task","脚本管理"},{"Kernel-PageFault","HardFault","内存"},
 {"Kernel-DiskIO","Read","磁盘"},{"Kernel-ALPC","Send","进程间通信"},{"Kernel-DebugEvents","Debug","调试"},
 {"Microsoft-Windows-Kernel-Process","ImageLoad","映像"},{"Vendor-Custom","RegisterProfile","通用"}};
 for(const auto& c:cases)if(inferEtwResourceType(QString::fromUtf8(c.provider),QString::fromUtf8(c.event))!=QString::fromUtf8(c.resource)){printf("resource mismatch: %s %s\n",c.provider,c.event);return 111;}
 ''',
 'catalog': r'''
 ULONG providerBytes=0;auto status=TdhEnumerateProviders(nullptr,&providerBytes);if(status!=ERROR_INSUFFICIENT_BUFFER)return 100;
 std::vector<unsigned char> providerBuffer(providerBytes);auto providers=reinterpret_cast<PROVIDER_ENUMERATION_INFO*>(providerBuffer.data());
 if(TdhEnumerateProviders(providers,&providerBytes)!=ERROR_SUCCESS)return 101;
 QJsonArray catalog;ULONG eventCount=0,availableProviders=0;
 for(const auto& preset:etwPresetProviderDescriptorList()){
   QJsonObject entry;entry.insert(QStringLiteral("provider"),preset.providerNameText);entry.insert(QStringLiteral("preset_category"),preset.categoryText);entry.insert(QStringLiteral("inferred_category"),etwInferProviderCategory(preset.providerNameText));
   const TRACE_PROVIDER_INFO* found=nullptr;
   for(ULONG i=0;i<providers->NumberOfProviders;++i){auto& p=providers->TraceProviderInfoArray[i];auto name=etwTextAtOffset(providerBuffer.data(),p.ProviderNameOffset);if(name.compare(preset.providerNameText,Qt::CaseInsensitive)==0){found=&p;break;}}
   if(!found){entry.insert(QStringLiteral("available"),false);catalog.append(entry);continue;}
   entry.insert(QStringLiteral("available"),true);entry.insert(QStringLiteral("guid"),guidToText(found->ProviderGuid));
   auto guid=found->ProviderGuid;ULONG eventBytes=0;status=TdhEnumerateManifestProviderEvents(&guid,nullptr,&eventBytes);entry.insert(QStringLiteral("enumeration_status"),static_cast<int>(status));
   if(status!=ERROR_INSUFFICIENT_BUFFER){catalog.append(entry);continue;}
   std::vector<unsigned char> eventBuffer(eventBytes);auto events=reinterpret_cast<PROVIDER_EVENT_INFO*>(eventBuffer.data());
   if(TdhEnumerateManifestProviderEvents(&guid,events,&eventBytes)!=ERROR_SUCCESS)return 102;
   ++availableProviders;QJsonArray definitions;
   for(ULONG i=0;i<events->NumberOfEvents;++i){auto descriptor=events->EventDescriptorsArray[i];ULONG bytes=0;
     status=TdhGetManifestEventInformation(&guid,&descriptor,nullptr,&bytes);if(status!=ERROR_INSUFFICIENT_BUFFER)continue;
     std::vector<unsigned char> buffer(bytes);auto info=reinterpret_cast<TRACE_EVENT_INFO*>(buffer.data());
     if(TdhGetManifestEventInformation(&guid,&descriptor,info,&bytes)!=ERROR_SUCCESS)continue;
     QString event=etwTextAtOffset(buffer.data(),info->EventNameOffset),task=etwTextAtOffset(buffer.data(),info->TaskNameOffset),opcode=etwTextAtOffset(buffer.data(),info->OpcodeNameOffset);
     if(event.isEmpty())event=task;if(event.isEmpty())event=opcode;
     QJsonObject definition;definition.insert(QStringLiteral("id"),descriptor.Id);definition.insert(QStringLiteral("version"),descriptor.Version);definition.insert(QStringLiteral("event"),event);definition.insert(QStringLiteral("task"),task);definition.insert(QStringLiteral("opcode"),opcode);definition.insert(QStringLiteral("opcode_value"),descriptor.Opcode);
     definition.insert(QStringLiteral("resource"),inferEtwResourceType(preset.providerNameText,event));definition.insert(QStringLiteral("action"),inferEtwActionText(event,opcode,preset.providerNameText,descriptor.Opcode));
     QJsonArray fields;for(ULONG j=0;j<info->TopLevelPropertyCount;++j){const auto& property=info->EventPropertyInfoArray[j];QString name=etwTextAtOffset(buffer.data(),property.NameOffset);QJsonObject field;field.insert(QStringLiteral("name"),name);field.insert(QStringLiteral("meaning"),etwPropertyMeaningText(normalizeEtwPropertyName(name)));fields.append(field);}definition.insert(QStringLiteral("fields"),fields);
     definitions.append(definition);++eventCount;
   }
   entry.insert(QStringLiteral("events"),definitions);catalog.append(entry);
 }
 const char* classics[]={"{2CB15D1D-5FC1-11D2-ABE1-00A0C911F518}","{3D6FA8D0-FE05-11D0-9DDA-00C04FD7BA7C}","{3D6FA8D1-FE05-11D0-9DDA-00C04FD7BA7C}","{9A280AC0-C8E0-11D1-84E2-00C04FB998A2}","{BF3A50C5-A9C9-4988-A005-2DF0B7C80F80}","{90CBDC39-4A3E-11D1-84F4-0000F80464E3}","{AE53722E-C863-11D2-8659-00C04FA321A1}","{3D6FA8D4-FE05-11D0-9DDA-00C04FD7BA7C}","{3D6FA8D3-FE05-11D0-9DDA-00C04FD7BA7C}","{CE1DBFB4-137E-4DA6-87B0-3F59AA102CBC}","{D837CA92-12B9-44A5-AD6A-3A65B3578AA8}","{45D8CCCD-539F-4B72-A8B7-5C683142609A}"};
 ULONG classicCount=0;
 for(const char* text:classics){auto guidText=QString::fromLatin1(text);auto wide=guidText.toStdWString();GUID guid{};CLSIDFromString(wide.c_str(),&guid);
   QJsonObject entry;entry.insert(QStringLiteral("classic_guid"),guidText);QJsonArray definitions;
   for(int version=0;version<=5;++version)for(int opcode=0;opcode<=255;++opcode){EVENT_RECORD r{};r.EventHeader.Flags=EVENT_HEADER_FLAG_CLASSIC_HEADER|EVENT_HEADER_FLAG_64_BIT_HEADER;r.EventHeader.ProviderId=guid;r.EventHeader.EventDescriptor.Version=version;r.EventHeader.EventDescriptor.Opcode=opcode;
     EtwSchemaEntry info;if(!tryBuildEtwSchemaByTdh(&r,&info))continue;auto name=etwProviderDisplayName(guidText,guidText,opcode);auto event=info.eventNameText.isEmpty()?info.taskNameText:info.eventNameText;
     QJsonObject definition;definition.insert(QStringLiteral("version"),version);definition.insert(QStringLiteral("provider"),name);definition.insert(QStringLiteral("event"),event);definition.insert(QStringLiteral("opcode"),info.opcodeNameText);definition.insert(QStringLiteral("opcode_value"),opcode);definition.insert(QStringLiteral("resource"),inferEtwResourceType(name,event));definition.insert(QStringLiteral("action"),inferEtwActionText(event,info.opcodeNameText,name,opcode));
     QJsonArray fields;for(const auto& p:info.propertyList){QJsonObject field;field.insert(QStringLiteral("name"),p.propertyNameText);field.insert(QStringLiteral("meaning"),p.meaningText);fields.append(field);}definition.insert(QStringLiteral("fields"),fields);definitions.append(definition);++classicCount;
   }
   entry.insert(QStringLiteral("events"),definitions);catalog.append(entry);
 }
 printf("CLASSIC CATALOG: event definitions=%lu\n",classicCount);
 QFile output(QString::fromLocal8Bit(qgetenv("KSWORD_ETW_CATALOG_OUTPUT")));if(!output.open(QIODevice::WriteOnly))return 103;
 output.write(QJsonDocument(catalog).toJson(QJsonDocument::Indented));printf("CATALOG: preset manifests=%lu events=%lu\n",availableProviders,eventCount);
 ''',
 'missing_identity': r'''
 record.EventHeader.EventDescriptor.Opcode=10;decoded.clear();auto missing=fill(guidToText(imageGuid),QStringLiteral("Image"),QStringLiteral("Load"));
 if(missing.securityPidValid||missing.securityTidValid||missing.pidTidText!=QStringLiteral("未知 / 未知"))return 90;
 auto semantic=inferEtwSemanticSummary(QStringLiteral("Kernel-Image"),QStringLiteral("Image"),QStringLiteral("Load"),decoded);
 auto summary=buildEtwSummaryText(QStringLiteral("Kernel-Image"),QStringLiteral("Image"),QStringLiteral("Load"),etwRelatedProcessId(missing),etwRelatedThreadId(missing),semantic,decoded);
 if(summary.contains(QStringLiteral("3212"))||summary.contains(QStringLiteral("4294967295"))||!summary.contains(QStringLiteral("PID=未知")))return 91;
 schema={};schema.propertyList={prop(0,"SubjectProcessId",TDH_INTYPE_UINT32),prop(1,"ProcessId",TDH_INTYPE_UINT32)};std::uint32_t audit[]={3212,2222};
 run(schema,audit,sizeof(audit));auto subject=fill(QStringLiteral("Security"),QStringLiteral("Audit"),QString());
 if(!subject.securityPidValid||subject.securityPid!=3212||etwRelatedProcessId(subject)!=2222)return 92;
 ''',
 'schema_cache': r'''
 EVENT_RECORD tl{};tl.EventHeader=record.EventHeader;tl.EventHeader.EventDescriptor.Channel=11;tl.EventHeader.EventDescriptor.Id=0;
 unsigned char one[]={5,0,'A',0,7},two[]={5,0,'B',0,8},copy[]={5,0,'A',0,7};
 EVENT_HEADER_EXTENDED_DATA_ITEM metadata{};metadata.ExtType=EVENT_HEADER_EXT_TYPE_EVENT_SCHEMA_TL;metadata.DataSize=sizeof(one);metadata.DataPtr=reinterpret_cast<ULONG_PTR>(one);
 tl.ExtendedData=&metadata;tl.ExtendedDataCount=1;auto first=etwSchemaKeyFromRecord(&tl);
 metadata.DataPtr=reinterpret_cast<ULONG_PTR>(two);auto second=etwSchemaKeyFromRecord(&tl);if(first==second)return 80;
 metadata.DataPtr=reinterpret_cast<ULONG_PTR>(copy);if(first!=etwSchemaKeyFromRecord(&tl))return 81;
 metadata.DataPtr=reinterpret_cast<ULONG_PTR>(one);tl.EventHeader.Flags=EVENT_HEADER_FLAG_32_BIT_HEADER;if(first==etwSchemaKeyFromRecord(&tl))return 82;
 ''',
 'id_bounds': r'''
 schema={};schema.propertyList={prop(0,"ProcessId",TDH_INTYPE_UINT64)};std::uint64_t oversized=0x100000001ULL;
 run(schema,&oversized,sizeof(oversized));std::uint32_t value=0;if(etwPropertyToUInt32(&decoded[0],&value))return 70;
 EtwDecodedPropertyEntry text;text.valueText=QStringLiteral("4294967297");if(etwPropertyToUInt32(&text,&value))return 71;
 text.valueText=QStringLiteral("0xffffffff");if(!etwPropertyToUInt32(&text,&value)||value!=0xffffffffU)return 72;
 schema.propertyList={prop(0,"ProcessId",TDH_INTYPE_INT8)};signed char negative=-1;
 run(schema,&negative,sizeof(negative));if(etwPropertyToUInt32(&decoded[0],&value))return 73;
 schema.propertyList={prop(0,"ProcessId",TDH_INTYPE_UINT32)};std::uint32_t valid=3212;
 run(schema,&valid,sizeof(valid));if(!etwPropertyToUInt32(&decoded[0],&value)||value!=3212)return 74;
 ''',
 'layout': r'''
 std::uint32_t array[]={11,22,333};schema={};schema.propertyList={prop(0,"Values",TDH_INTYPE_UINT32,2),prop(1,"ProcessId",TDH_INTYPE_UINT32)};
 if(!run(schema,array,sizeof(array))||parsed!=12||!decoded[1].numericAvailable||decoded[1].numericValue!=333||decoded[0].numericAvailable)return 50;
 std::uint32_t zero[]={0,444};schema.propertyList={prop(0,"Count",TDH_INTYPE_UINT32),prop(1,"Values",TDH_INTYPE_UINT32),prop(2,"ProcessId",TDH_INTYPE_UINT32)};
 schema.propertyList[1].useCountProperty=true;schema.propertyList[1].countPropertyIndex=0;
 if(!run(schema,zero,sizeof(zero))||parsed!=8||decoded[2].numericValue!=444||decoded[1].beginOffset!=decoded[1].endOffset)return 51;
 std::uint32_t structure[]={11,22,555};schema.propertyList={prop(0,"Envelope",TDH_INTYPE_NULL),prop(1,"ProcessId",TDH_INTYPE_UINT32),prop(2,"ProcessId",TDH_INTYPE_UINT32),prop(3,"B",TDH_INTYPE_UINT32)};
 schema.topLevelPropertyCount=2;schema.propertyList[0].isStruct=true;schema.propertyList[0].structStartIndex=2;schema.propertyList[0].structMemberCount=2;
 if(!run(schema,structure,sizeof(structure))||parsed!=12||decoded.size()!=3||decoded[2].numericValue!=555)return 52;
 auto pid=findFirstEtwProperty(decoded,QStringList{QStringLiteral("processid")});if(!pid||pid->numericValue!=555)return 53;
 schema.propertyList[0].structMemberCount=0;
 if(run(schema,structure,sizeof(structure))||parsed!=0||tail.isEmpty()||findFirstEtwProperty(decoded,QStringList{QStringLiteral("processid")}))return 54;
 schema={};schema.propertyList={prop(0,"Counted",TDH_INTYPE_COUNTEDSTRING),prop(1,"ProcessId",TDH_INTYPE_UINT32)};
 unsigned char counted[]={4,0,'A',0,'B',0,0x9a,2,0,0};
 if(!run(schema,counted,sizeof(counted))||parsed!=10||decoded[1].numericValue!=666||decoded[0].valueText!=QStringLiteral("AB"))return 55;
 unsigned char truncated[]={100,0,'A',0,0,0};
 if(run(schema,truncated,sizeof(truncated))||parsed!=0||tail.isEmpty()||decoded.size()!=1)return 56;
 schema.propertyList={prop(0,"Length",TDH_INTYPE_UINT32),prop(1,"Name",TDH_INTYPE_UNICODESTRING),prop(2,"ProcessId",TDH_INTYPE_UINT32)};
 schema.propertyList[1].useLengthProperty=true;schema.propertyList[1].lengthPropertyIndex=0;
 if(!run(schema,zero,sizeof(zero))||parsed!=8||decoded[2].numericValue!=444)return 57;
 schema.propertyList={prop(0,"Names",TDH_INTYPE_UNICODESTRING,2),prop(1,"ProcessId",TDH_INTYPE_UINT32)};
 unsigned char names[]={'A',0,0,0,'B',0,0,0,0x9a,2,0,0};
 if(!run(schema,names,sizeof(names))||parsed!=12||decoded[1].numericValue!=666)return 58;
 schema.propertyList={prop(0,"Sid",TDH_INTYPE_SID),prop(1,"ProcessId",TDH_INTYPE_UINT32)};
 unsigned char sid[]={1,15,0,0,0,0,0,5,0,0,0,0};
 if(run(schema,sid,sizeof(sid))||parsed!=0||decoded.size()!=1)return 59;
 schema.propertyList={prop(0,"Unknown",0xffff),prop(1,"ProcessId",TDH_INTYPE_UINT32)};
 if(run(schema,array,sizeof(array))||parsed!=0||decoded.size()!=1||tail.isEmpty())return 61;
 schema.propertyList={prop(0,"Length",TDH_INTYPE_UINT32),prop(1,"Name",TDH_INTYPE_UNICODESTRING),prop(2,"ProcessId",TDH_INTYPE_UINT32)};
 schema.propertyList[1].useLengthProperty=true;schema.propertyList[1].lengthPropertyIndex=0;
 unsigned char fixed[]={2,0,0,0,'A',0,'B',0,0x9a,2,0,0};
 auto ok=run(schema,fixed,sizeof(fixed));if(!ok||parsed!=12||decoded.size()<3||decoded[2].numericValue!=666||decoded[1].valueText!=QStringLiteral("AB")){printf("fixed string ok=%d parsed=%lu size=%zu text=%s\n",ok,parsed,decoded.size(),qPrintable(decoded[1].valueText));return 62;}
 ''',
 'ports': r'''
 unsigned char ports[]={0x01,0xbb,0x00,0x50};schema.propertyList={prop(0,"sport",TDH_INTYPE_UINT16),prop(1,"dport",TDH_INTYPE_UINT16)};
 for(auto& p:schema.propertyList)p.outType=TDH_OUTTYPE_PORT;
 run(schema,ports,sizeof(ports));auto network=fill(QStringLiteral("Kernel-TCPIP"),QStringLiteral("Send"),QString());
 if(!network.sourcePortValid||network.sourcePort!=443||!network.destinationPortValid||network.destinationPort!=80||parsed!=4)return 40;
 TRACE_EVENT_INFO info{};ULONG bytes=64;USHORT consumed=0;wchar_t formatted[32]{};
 auto status=TdhFormatProperty(&info,nullptr,8,TDH_INTYPE_UINT16,TDH_OUTTYPE_PORT,2,2,ports,&bytes,formatted,&consumed);if(status!=ERROR_SUCCESS||QString::fromWCharArray(formatted)!=QStringLiteral("443")||consumed!=2){printf("port TDH status=%lu consumed=%u value=%s\n",status,consumed,qPrintable(QString::fromWCharArray(formatted)));return 41;}
 ''',
 'thread_identity': r'''
 std::uint32_t thread[]={24680,22222};schema.propertyList={prop(0,"ProcessId",TDH_INTYPE_UINT32),prop(1,"TThreadId",TDH_INTYPE_UINT32)};
 record.EventHeader.ProviderId={0x3d6fa8d1,0xfe05,0x11d0,{0x9d,0xda,0,0xc0,0x4f,0xd7,0xba,0x7c}};
 run(schema,thread,sizeof(thread));auto tr=fill(QStringLiteral("Thread"),QStringLiteral("Thread"),QStringLiteral("End"));
 if(!tr.targetTidValid||etwRelatedThreadId(tr)!=22222||etwRelatedProcessId(tr)!=24680)return 30;
 std::uint32_t ttid=22222;schema.propertyList={prop(0,"TTID",TDH_INTYPE_UINT32)};
 record.EventHeader.ProviderId={0x90cbdc39,0x4a3e,0x11d1,{0x84,0xf4,0,0,0xf8,4,0x64,0xe3}};
 run(schema,&ttid,sizeof(ttid));auto file=fill(QStringLiteral("Kernel-FileIO"),QStringLiteral("Read"),QString());
 if(!file.targetTidValid||file.targetTid!=22222||etwRelatedProcessId(file)!=std::numeric_limits<std::uint32_t>::max())return 31;
 if(file.pidTidText!=QStringLiteral("未知 / 22222"))return 32;
 ''',
 'semantics': r'''
 if(row.resourceTypeText!=QStringLiteral("映像")||row.actionText!=QStringLiteral("枚举开始")||row.imagePathText.isEmpty())return 20;
 if(etwProviderDisplayName(guidToText(imageGuid),QString())!=QStringLiteral("Microsoft-Windows-Kernel-Image"))return 21;
 if(inferEtwActionText(QStringLiteral("Thread"),QStringLiteral("End"))!=QStringLiteral("结束"))return 22;
 if(inferEtwActionText(QStringLiteral("Thread"),QStringLiteral("DCStart"))!=QStringLiteral("枚举开始"))return 23;
 if(inferEtwActionText(QStringLiteral("Image"),QStringLiteral("Unload"))!=QStringLiteral("卸载"))return 24;
 if(inferEtwActionText(QStringLiteral("DisconnectIPV4"),QString())!=QStringLiteral("断开连接"))return 25;
 if(inferEtwActionText(QStringLiteral("FileIoRead"),QString())!=QStringLiteral("读取/查询"))return 26;
 ''',
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
selected=[name for name in tests if name!='catalog'] if args.case=='all' else [args.case]
if args.catalog_output:
 args.catalog_output.parent.mkdir(parents=True,exist_ok=True)
 selected.append('catalog')
if any(name not in tests for name in selected):parser.error('Unknown case')
reset='record.EventHeader.ProviderId=imageGuid;record.EventHeader.EventDescriptor.Opcode=3;run(installed,image,sizeof(image));row=fill(guidToText(imageGuid),QStringLiteral("Image"),QStringLiteral("DCStart"));'
body='\n'.join('{'+reset+tests[name]+'}' for name in selected)
code=code.replace('TEST_BODY',body).replace('CASE_NAME',','.join(selected))

with tempfile.TemporaryDirectory(prefix='ksword_etw_decode_') as temp:
 d=Path(temp);cpp=d/'audit.cpp';cpp.write_text(code,encoding='utf-8')
 compiler_args=['cl','/nologo','/permissive-','/EHsc','/std:c++17','/Zc:__cplusplus','/utf-8','/MD','/I'+str(qt/'include'),'/I'+str(qt/'include/QtCore'),str(cpp),'/Fe:'+str(d/'audit.exe'),'/Fo:'+str(d/'audit.obj'),'/link',str(qt/'lib/Qt6Core.lib'),'tdh.lib','advapi32.lib','ole32.lib']
 bat=d/'build.cmd';bat.write_text('@echo off\ncall "'+str(vsdev)+'" -arch=x64 -host_arch=x64 >nul\n'+subprocess.list2cmdline(compiler_args)+'\n',encoding='utf-8')
 r=subprocess.run(['cmd','/d','/c',str(bat)],cwd=temp,capture_output=True,text=True,encoding='utf-8',errors='replace');print(r.stdout);print(r.stderr)
 if r.returncode:raise SystemExit(r.returncode)
 subprocess.run([str(qt/'bin/windeployqt.exe'),'--release','--compiler-runtime','--no-opengl-sw',str(d/'audit.exe')],check=True,stdout=subprocess.DEVNULL)
 environment=os.environ.copy()
 if args.catalog_output:environment['KSWORD_ETW_CATALOG_OUTPUT']=str(args.catalog_output.resolve())
 subprocess.run([str(d/'audit.exe')],check=True,timeout=120 if args.catalog_output else 30,env=environment)
