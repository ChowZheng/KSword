#pragma once

#include <ntddk.h>

#include "../../../../shared/driver/KswordArkBugcheckIoctl.h"

// 回放只替换驱动框架入口，模式与报文继续引用生产共享协议，避免测试复制常量。
typedef struct _KSWORD_ARK_BUGCHECK_DIAGNOSTICS_REQUEST
    KSWORD_ARK_BUGCHECK_DIAGNOSTICS_REQUEST;
typedef struct _KSWORD_ARK_BUGCHECK_DIAGNOSTICS_RESPONSE
    KSWORD_ARK_BUGCHECK_DIAGNOSTICS_RESPONSE;
