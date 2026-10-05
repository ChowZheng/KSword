#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "driver/KswordArkHandleIoctl.h"

EXTERN_C_START

// Close one enumerated user handle and separately report temporary-suspend recovery.
NTSTATUS KswordARKDriverCloseHandle(_In_ const KSWORD_ARK_CLOSE_HANDLE_REQUEST* Request, _Out_ NTSTATUS* ResumeStatus);

NTSTATUS
KswordARKDriverEnumerateProcessHandles(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

NTSTATUS
KswordARKDriverQueryHandleObject(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
