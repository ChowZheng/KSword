#pragma once

#include "KswordArkHvmIoctl.h"

// EPT debug stops are distinct from the non-stopping, first-touch Memory Watch.
#define KSWORD_ARK_HVM_DEBUG_VERSION 1UL
#define KSWORD_ARK_IOCTL_FUNCTION_HVM_DEBUG 0x91EUL
#define IOCTL_KSWORD_ARK_HVM_DEBUG \
    CTL_CODE(KSWORD_ARK_IOCTL_DEVICE_TYPE, KSWORD_ARK_IOCTL_FUNCTION_HVM_DEBUG, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define KSWORD_ARK_HVM_DEBUG_QUERY 0UL
#define KSWORD_ARK_HVM_DEBUG_ADD 1UL
#define KSWORD_ARK_HVM_DEBUG_REMOVE 2UL
#define KSWORD_ARK_HVM_DEBUG_REVOKE 3UL
#define KSWORD_ARK_HVM_DEBUG_MAX_SLOTS 128UL
#define KSWORD_ARK_HVM_DEBUG_FLAG_CONFIRMED 1UL

typedef struct _KSWORD_ARK_HVM_DEBUG_REQUEST
{
    unsigned long version;
    unsigned long size;
    unsigned long operation;
    unsigned long flags;
    unsigned long processId;
    unsigned long threadId;
    unsigned long breakpointId;
    unsigned long debugRegister;
    unsigned long access;
    unsigned long length;
    unsigned long long address;
} KSWORD_ARK_HVM_DEBUG_REQUEST;

typedef struct _KSWORD_ARK_HVM_DEBUG_RESPONSE
{
    unsigned long version;
    unsigned long size;
    long status;
    unsigned long breakpointId;
    unsigned long ruleId;
    unsigned long activeCount;
    unsigned long supported;
    unsigned long reserved;
} KSWORD_ARK_HVM_DEBUG_RESPONSE;

// Execution stops match RIP exactly. Data stops deliberately use EPT's 4 KiB
// granularity; address/length preserve the debugger's requested range only.
static __inline int KswordArkHvmDebugMatch(
    unsigned long access, unsigned long requestedAccess,
    unsigned long debugRegister, unsigned long long dr7,
    unsigned long long rflags, unsigned long cpl,
    unsigned long long teb, unsigned long long targetTeb,
    unsigned long long rip, unsigned long long targetAddress)
{
    if (cpl != 3UL || teb == 0ULL || teb != targetTeb || debugRegister >= 4UL ||
        (dr7 & (3ULL << (debugRegister * 2UL))) == 0ULL ||
        (access & requestedAccess) == 0UL) { return 0; }
    if ((requestedAccess & KSWORD_ARK_HVM_EPT_ACCESS_EXECUTE) != 0UL) {
        return rip == targetAddress && (rflags & 0x10000ULL) == 0ULL;
    }
    return 1;
}
