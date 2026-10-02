"""Exercise production packet reads on fragmented and truncated native byte pipes."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
code=r"""
#include "PROTOCOL_PATH"
#include <thread>
#include <cstdio>
using namespace ks::winapi_monitor;
int test(bool truncated) {
 auto name=L"\\\\.\\pipe\\KswordPacketTest_"+std::to_wstring(GetCurrentProcessId())+(truncated?L"_t":L"_f");
 HANDLE server=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_OUTBOUND,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,1,65536,65536,0,nullptr);
 std::thread writer([&]{
  ConnectNamedPipe(server,nullptr);ApiMonitorEventPacket packet;packet.pid=123;DWORD n=0;
  WriteFile(server,&packet,100,&n,nullptr);Sleep(50);
  if(!truncated) {
   WriteFile(server,reinterpret_cast<unsigned char*>(&packet)+100,sizeof(packet)-100,&n,nullptr);
   packet.pid=456;WriteFile(server,&packet,sizeof(packet),&n,nullptr);
  }
  FlushFileBuffers(server);DisconnectNamedPipe(server);CloseHandle(server);
 });
 HANDLE client=CreateFileW(name.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
 std::atomic_bool stop{false}; ApiMonitorEventPacket packet;int count=0;
 while(readEventPacket(client,&packet,stop)) {
  if(packet.size!=sizeof(packet)||packet.version!=kProtocolVersion||packet.pid!=(count?456:123)) return 1;
  ++count;
 }
 CloseHandle(client);writer.join();
 return count==(truncated?0:2)?0:2;
}
int main() {
 if(test(false)||test(true)) return 1;
 std::atomic_bool stopped{true};ApiMonitorEventPacket packet;
 if(readEventPacket(INVALID_HANDLE_VALUE,&packet,stopped)) return 2;
 printf("PASS: fragmented packets preserved; truncated EOF and stop rejected\n");
}
"""
code=code.replace("PROTOCOL_PATH",(root/"shared/WinApiMonitorProtocol.h").as_posix())
with tempfile.TemporaryDirectory(prefix="ksword_packets_") as temp:
 folder=Path(temp);cpp=folder/"test.cpp";cpp.write_text(code,encoding="utf-8")
 subprocess.run(["cl","/nologo","/EHsc","/std:c++17","/utf-8",str(cpp),"/Fe:"+str(folder/"test.exe"),"/Fo:"+str(folder/"test.obj")],check=True)
 subprocess.run([str(folder/"test.exe")],check=True,timeout=30)
