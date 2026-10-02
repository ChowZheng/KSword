"""Exercise production queue overflow, control reserve and loss notification over a real pipe."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
code=r'''
#include <thread>
#include <cstdio>
#include "SOURCE_PATH"
namespace apimon {
 const MonitorConfig& ActiveConfig(){static MonitorConfig value;return value;}
 bool StopRequested(){return false;}
 bool IsStopFlagPresent(const MonitorConfig&){return false;}
 ScopedInlineHookInternalBypass::ScopedInlineHookInternalBypass(){}
 ScopedInlineHookInternalBypass::~ScopedInlineHookInternalBypass(){}
}
int main(){
 using namespace apimon;using namespace ks::winapi_monitor;
 for(size_t i=0;i<kMaxPendingPacketCount-kControlPacketReserve;++i)
  if(!SendMonitorEventRaw(EventCategory::File,L"test",L"event",0,L""))return 1;
 for(int i=0;i<73;++i)if(SendMonitorEventRaw(EventCategory::File,L"test",L"event",0,L""))return 2;
 for(size_t i=0;i<kControlPacketReserve;++i)
  if(!SendMonitorEventRaw(EventCategory::Internal,L"Agent",L"HooksInstalled",0,L""))return 3;
 if(SendMonitorEventRaw(EventCategory::Internal,L"Agent",L"excess",0,L""))return 4;
 if(g_droppedPacketCount.load()!=74)return 5;
 FlushPendingMonitorEvents(10);
 if(g_droppedPacketCount.load()!=84)return 6;
 auto name=buildPipeNameForPid(GetCurrentProcessId())+L"_loss_test";
 HANDLE server=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_OUTBOUND|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_WAIT,1,65536,65536,0,nullptr);
 HANDLE client=CreateFileW(name.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
 if(server==INVALID_HANDLE_VALUE||client==INVALID_HANDLE_VALUE)return 7;
 g_pipeHandle=server;g_senderStopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 if(FlushPendingMonitorEvents(2)!=2)return 8;
 ApiMonitorEventPacket notice{};DWORD read=0;
 if(!ReadFile(client,&notice,sizeof(notice),&read,nullptr)||read!=sizeof(notice))return 9;
 if(wcscmp(notice.apiName,L"EventsDropped")||wcscmp(notice.detailText,L"84"))return 10;
 for(int i=0;i<2;++i)if(!ReadFile(client,&notice,sizeof(notice),&read,nullptr)||read!=sizeof(notice))return 11;
 StopMonitorPipeServer();CloseHandle(client);CloseHandle(g_senderStopEvent);g_senderStopEvent=nullptr;
 if(g_droppedPacketCount.load()!=0||g_pendingPacketCount!=0)return 12;
 printf("PASS: bounded overflow, internal reserve, unsent accounting, loss notice and reset\n");
}
'''.replace("SOURCE_PATH",(root/"APIMonitor_x64/core/MonitorPipe.cpp").as_posix())
with tempfile.TemporaryDirectory(prefix="ksword_loss_") as temp:
 folder=Path(temp);cpp=folder/"fixture.cpp";cpp.write_text(code,encoding="utf-8")
 subprocess.run(["cl","/nologo","/EHsc","/std:c++17","/utf-8","/I"+str(root/"APIMonitor_x64"),str(cpp),"/Fe:"+str(folder/"fixture.exe"),"/Fo:"+str(folder/"fixture.obj")],check=True)
 subprocess.run([str(folder/"fixture.exe")],check=True,timeout=30)
