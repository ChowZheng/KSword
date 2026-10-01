#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Controlled guest-only target: three isolated pages, no third-party process. */
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    volatile uint64_t* control = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    volatile uint64_t* data = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    unsigned char* code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!control || !data || !code) return 3;
    /* mov rax,rcx; inc rax; mov [absolute data],rax; ret */
    unsigned char program[] = {0x48,0x89,0xc8,0x48,0xff,0xc0,0x48,0xa3,0,0,0,0,0,0,0,0,0xc3};
    uint64_t dataAddress = (uint64_t)(uintptr_t)data;
    memcpy(program+8, &dataAddress, sizeof(dataAddress));
    memcpy(code, program, sizeof(program));
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(program));
    char path[MAX_PATH];
    sprintf_s(path,sizeof(path),"%s\\target.json",argv[1]);
    FILE* info = NULL;
    if (fopen_s(&info,path,"w") || !info) return 4;
    fprintf(info,"{\"pid\":%lu,\"tid\":%lu,\"control\":%llu,\"data\":%llu,\"code\":%llu}\n",
        GetCurrentProcessId(),GetCurrentThreadId(),(uint64_t)(uintptr_t)control,
        dataAddress,(uint64_t)(uintptr_t)code);
    fclose(info);
    typedef uint64_t (*Probe)(uint64_t);
    Probe probe = (Probe)code;
    for (;;) {
        uint64_t command = control[0];
        if (command == 9) break;
        if (command == 1) { control[0]=0; control[1]=probe(0x100); control[2]++; }
        else if (command == 2) { control[0]=0; data[0]=0x5a; control[2]++; }
        else if (command == 3) { control[0]=0; control[1]=data[0]; control[2]++; }
        Sleep(20);
    }
    VirtualFree(code,0,MEM_RELEASE);
    VirtualFree((void*)data,0,MEM_RELEASE);
    VirtualFree((void*)control,0,MEM_RELEASE);
    return 0;
}
