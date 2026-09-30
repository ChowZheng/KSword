#pragma once

#include "KswordArkProcessIoctl.h"

// Debugger-neutral R0 operations. Context bytes use Windows AMD64 CONTEXT layout.
#define KSWORD_ARK_DEBUGGER_VERSION 1UL
#define KSWORD_ARK_IOCTL_FUNCTION_DEBUGGER 0x91FUL
#define IOCTL_KSWORD_ARK_DEBUGGER \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_DEBUGGER, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define KSWORD_ARK_DEBUGGER_QUERY 0UL
#define KSWORD_ARK_DEBUGGER_GET_CONTEXT 1UL
#define KSWORD_ARK_DEBUGGER_SET_CONTEXT 2UL
#define KSWORD_ARK_DEBUGGER_SUSPEND 3UL
#define KSWORD_ARK_DEBUGGER_RESUME 4UL
#define KSWORD_ARK_DEBUGGER_ALLOCATE 5UL
#define KSWORD_ARK_DEBUGGER_PROTECT 6UL
#define KSWORD_ARK_DEBUGGER_FREE 7UL
#define KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED 1UL
#define KSWORD_ARK_DEBUGGER_CAP_CONTEXT 1UL
#define KSWORD_ARK_DEBUGGER_CAP_SUSPEND 2UL
#define KSWORD_ARK_DEBUGGER_CAP_MEMORY 4UL
#define KSWORD_ARK_DEBUGGER_CONTEXT_BYTES 1232UL

typedef struct _KSWORD_ARK_DEBUGGER_REQUEST
{
    unsigned long version;
    unsigned long size;
    unsigned long operation;
    unsigned long flags;
    unsigned long processId;
    unsigned long threadId;
    unsigned long contextFlags;
    unsigned long reserved;
    unsigned long long address;
    unsigned long long bytes;
    unsigned long long reservedArgument;
    unsigned long protection;
    unsigned long allocationType;
    unsigned char context[KSWORD_ARK_DEBUGGER_CONTEXT_BYTES];
} KSWORD_ARK_DEBUGGER_REQUEST;

typedef struct _KSWORD_ARK_DEBUGGER_RESPONSE
{
    unsigned long version;
    unsigned long size;
    long status;
    unsigned long capabilities;
    unsigned long processId;
    unsigned long threadId;
    unsigned long previousSuspendCount;
    unsigned long previousProtection;
    unsigned long long address;
    unsigned long long bytes;
    unsigned long long reserved0;
    unsigned long long reserved1;
    unsigned char context[KSWORD_ARK_DEBUGGER_CONTEXT_BYTES];
} KSWORD_ARK_DEBUGGER_RESPONSE;
