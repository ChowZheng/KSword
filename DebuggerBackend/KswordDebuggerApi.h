#pragma once

#include <stdint.h>

// In-process, versioned debugger API. Wire structures live only in shared/driver.
// A debugger adapter supplies IDs and addresses; it never opens KswordARK itself.
#define KSWORD_DEBUGGER_API_VERSION 1U
#define KSWORD_DEBUGGER_QUERY_BACKEND 1U
#define KSWORD_DEBUGGER_USE_HVM 2U
#define KSWORD_DEBUGGER_OPERATION_LAYOUT 3U
#define KSWORD_DEBUGGER_GET_OPTIONS 4U
#define KSWORD_DEBUGGER_SET_OPTIONS 5U
#define KSWORD_DEBUGGER_QUERY_POLICY 6U
#define KSWORD_DEBUGGER_RESTORE_SHADOW_WRITES 7U
#define KSWORD_DEBUGGER_OPTIONS_VERSION 1U
#define KSWORD_DEBUGGER_MODE_NORMAL 0U
#define KSWORD_DEBUGGER_MODE_STEALTH 1U
#define KSWORD_DEBUGGER_PATH_NATIVE 0U
#define KSWORD_DEBUGGER_PATH_EPT 1U
#define KSWORD_DEBUGGER_PATH_SHADOW 2U
#define KSWORD_DEBUGGER_HVM_STATUS 16U
#define KSWORD_DEBUGGER_HVM_CONTROL 17U
#define KSWORD_DEBUGGER_HVM_MEMORY 18U
#define KSWORD_DEBUGGER_HVM_EPT_RULE 19U
#define KSWORD_DEBUGGER_HVM_EVENTS 20U
#define KSWORD_DEBUGGER_HVM_VIEW 21U
#define KSWORD_DEBUGGER_HVM_CR_POLICY 22U
#define KSWORD_DEBUGGER_HVM_MSR_POLICY 23U
#define KSWORD_DEBUGGER_HVM_DOMAIN 24U
#define KSWORD_DEBUGGER_HVM_PROCESS 25U
#define KSWORD_DEBUGGER_HVM_INJECT 26U
#define KSWORD_DEBUGGER_HVM_PLATFORM 27U
#define KSWORD_DEBUGGER_HVM_METRICS 28U
#define KSWORD_DEBUGGER_HVM_NESTED_PROBE 29U
#define KSWORD_DEBUGGER_HVM_NESTED_PAGE 30U
#define KSWORD_DEBUGGER_HVM_BREAKPOINT 31U
#define KSWORD_DEBUGGER_NATIVE 32U

// All pointers are encoded as uint64_t so the ABI is stable across adapters.
typedef struct KSWORD_DEBUGGER_CALL
{
    uint32_t version;
    uint32_t size;
    uint32_t command;
    uint32_t reserved;
    uint64_t input;
    uint64_t output;
    uint32_t inputBytes;
    uint32_t outputBytes;
    uint32_t error;
    uint32_t bytesReturned;
} KSWORD_DEBUGGER_CALL;

typedef struct KSWORD_DEBUGGER_BACKEND_STATUS
{
    uint32_t version;
    uint32_t size;
    uint32_t driverReady;
    uint32_t useHvm;
    uint32_t directMemoryWindow;
    uint32_t residentActive;
    uint32_t ownsResident;
    uint32_t attachedProcessId;
    uint32_t eptBreakpointProtocol;
    uint32_t lastError;
} KSWORD_DEBUGGER_BACKEND_STATUS;

// Old adapters retain normal mode and ordinary memory writes. Each boolean is
// 0 or 1; critical fallback logging is required, so logFallback must be 1.
typedef struct KSWORD_DEBUGGER_OPTIONS
{
    uint32_t version;
    uint32_t size;
    uint32_t mode;
    uint32_t shadowMemoryWrites;
    uint32_t allowFallback;
    uint32_t logFallback;
    uint32_t nativeContextFallback;
    uint32_t nativeSuspendFallback;
    uint32_t maxShadowPages;
    uint32_t reserved[3];
} KSWORD_DEBUGGER_OPTIONS;

typedef struct KSWORD_DEBUGGER_POLICY_STATUS
{
    KSWORD_DEBUGGER_OPTIONS options;
    uint32_t activePath;
    uint32_t shadowWritePages;
    uint32_t activeBreakpoints;
    uint32_t canChangeOptions;
    uint32_t fallbackCount;
    uint32_t lastFallbackError;
} KSWORD_DEBUGGER_POLICY_STATUS;

// address=bytes=0 restores all memory patches owned by this adapter. Otherwise
// the range is restored in every owned target; hidden breakpoints are retained.
typedef struct KSWORD_DEBUGGER_SHADOW_RESTORE
{
    uint32_t version;
    uint32_t size;
    uint64_t address;
    uint64_t bytes;
} KSWORD_DEBUGGER_SHADOW_RESTORE;

typedef struct KSWORD_DEBUGGER_OPERATION_INFO
{
    uint32_t version;
    uint32_t command;
    uint32_t inputBytes;
    uint32_t outputBytes;
    uint32_t protocolVersion;
    uint32_t reserved;
} KSWORD_DEBUGGER_OPERATION_INFO;

#ifdef __cplusplus
static_assert(sizeof(KSWORD_DEBUGGER_CALL) == 48, "Debugger ABI layout");
static_assert(sizeof(KSWORD_DEBUGGER_OPTIONS) == 48, "Debugger options ABI layout");
static_assert(sizeof(KSWORD_DEBUGGER_POLICY_STATUS) == 72, "Debugger policy ABI layout");
static_assert(sizeof(KSWORD_DEBUGGER_SHADOW_RESTORE) == 24, "Debugger restore ABI layout");
extern "C" {
#endif
#ifdef _WIN32
unsigned long __stdcall KSwordDebuggerCall(KSWORD_DEBUGGER_CALL* call);
#endif
#ifdef __cplusplus
}
#endif
