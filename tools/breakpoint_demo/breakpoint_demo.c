#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <psapi.h>

volatile LONG counter = 0;

#pragma code_seg(push, ".kswdemo")
__declspec(noinline) void increment(void)
{
    counter++;
}
#pragma code_seg(pop)

static int prepare_demo_page(void)
{
    volatile unsigned char *code = (volatile unsigned char *)(uintptr_t)&increment;
    DWORD original, ignored;
    PSAPI_WORKING_SET_EX_INFORMATION page = {0};
    unsigned char byte;
    if (!VirtualProtect((void *)code, 1, PAGE_EXECUTE_WRITECOPY, &original)) return 0;
    byte = *code;
    *code = byte; /* Same-byte write: Windows makes only this process's page private. */
    if (!VirtualProtect((void *)code, 1, original, &ignored)) return 0;
    if (!FlushInstructionCache(GetCurrentProcess(), (void *)code, 1)) return 0;
    page.VirtualAddress = (void *)code;
    if (!QueryWorkingSetEx(GetCurrentProcess(), &page, sizeof(page))) return 0;
    if (!page.VirtualAttributes.Valid || page.VirtualAttributes.Shared) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    return 1;
}

int main(void)
{
    int ch;
    if (!prepare_demo_page()) {
        printf("Cannot prepare private demo code page: error %lu\n", GetLastError());
        puts("Press Enter to quit.");
        (void)getchar();
        return 1;
    }
    printf("PID: %lu\n", GetCurrentProcessId());
    puts("Shadow-ready demo code page: Valid=1, Shared=0 (original bytes preserved).");
    printf("Execution breakpoint: increment = %p\n", (void *)(uintptr_t)&increment);
    printf("Data write breakpoint: counter = %p, size = 4 bytes\n", (void *)&counter);
    puts("Enter: increment once. q + Enter: quit.");
    while ((ch = getchar()) != EOF) {
        if (ch == 'q' || ch == 'Q') break;
        if (ch == '\n') {
            increment();
            printf("counter = %ld\n", counter);
        }
    }
    return 0;
}
