"""Network completion observers remain available when file event capture is disabled."""
from live_fixture_support import LiveFixture

source = r'''
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <cstdio>
#include <cstring>
int Run(){WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa))return 10;
 SOCKET listener=WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,0,WSA_FLAG_OVERLAPPED),sender=WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,0,WSA_FLAG_OVERLAPPED);
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(listener,1))return 11;
 int length=sizeof(address);getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length);if(connect(sender,reinterpret_cast<sockaddr*>(&address),length))return 12;
 SOCKET receiver=accept(listener,nullptr,nullptr);HANDLE port=CreateIoCompletionPort(reinterpret_cast<HANDLE>(receiver),nullptr,123,0);if(!port)return 13;
 char buffer[8]{};WSABUF input{8,buffer};OVERLAPPED ov{};DWORD bytes=0,flags=0;
 if(WSARecv(receiver,&input,1,&bytes,&flags,&ov,nullptr)!=SOCKET_ERROR||WSAGetLastError()!=WSA_IO_PENDING)return 14;
 if(send(sender,"fixture",8,0)!=8)return 15;
 ULONG_PTR key=0;OVERLAPPED* completed=nullptr;
 if(!GetQueuedCompletionStatus(port,&bytes,&key,&completed,5000)||bytes!=8||completed!=&ov||key!=123||memcmp(buffer,"fixture",8))return 16;
 closesocket(receiver);closesocket(sender);closesocket(listener);CloseHandle(port);WSACleanup();return 0;}
int wmain(int argc,wchar_t** argv){if(argc!=2)return 1;printf("%lu\n",GetCurrentProcessId());fflush(stdout);
 for(int c;(c=getchar())!=EOF;){if(c=='L'){if(!LoadLibraryW(argv[1]))return 2;}if(c=='X'){int result=Run();if(result)return result;printf("DONE\n");fflush(stdout);}if(c=='Q')return 0;}return 0;}
'''
with LiveFixture(source, {"enable_file": 0, "enable_network": 1}) as fixture:
    fixture.wait_for(lambda f: bool(f.snapshots) and any(e.api == "HooksInstalled" for e in f.events))
    fixture.command("X")
    fixture.wait_for(lambda f: any(e.api == "WSARecv" and e.kind == 3 for e in f.events))
    assert fixture.process.stdout.readline().strip() == b"DONE"
    assert all(e.category != 1 for e in fixture.events)
    completion = next(e for e in fixture.events if e.api == "WSARecv" and e.kind == 3)
    assert completion.result == 0 and completion.operation and "bytes=8" in completion.detail
print("PASS: network-only IOCP correlation with file category disabled")
