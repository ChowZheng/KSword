[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)

$ErrorActionPreference = 'Stop'

function Get-CCodeText {
    param([Parameter(Mandatory)] [string]$Text)

    # Preserve offsets, but exclude braces/call names in comments and literals.
    return [regex]::Replace($Text,
        '//[^\r\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*''',
        [System.Text.RegularExpressions.MatchEvaluator] {
            param($match)
            return [regex]::Replace($match.Value, '[^\r\n]', ' ')
        })
}

function Get-CFunctionBody {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Name
    )

    $text = Get-CCodeText -Text (Get-Content -LiteralPath $Path -Raw)
    # Require a definition, not a forward declaration or a call site. StartCore
    # is declared before DriverEntry, so selecting its first name is unsafe.
    $definitions = [regex]::Matches($text,
        '(?m)^\s*(?:[A-Za-z_]\w*\s+)+' + [regex]::Escape($Name) +
        '\s*\([^;{}]*\)\s*(?<BodyStart>\{)')
    if ($definitions.Count -ne 1) {
        throw "Expected one definition of '$Name' in '$Path'; found $($definitions.Count)."
    }
    $bodyStart = $definitions[0].Groups['BodyStart'].Index

    $depth = 0
    for ($index = $bodyStart; $index -lt $text.Length; ++$index) {
        switch ($text[$index]) {
            '{' { ++$depth }
            '}' {
                --$depth
                if ($depth -eq 0) {
                    return $text.Substring($bodyStart, $index - $bodyStart + 1)
                }
            }
        }
    }
    throw "Function '$Name' has an unterminated body in '$Path'."
}

function Assert-Matches {
    param(
        [Parameter(Mandatory)] [string]$Text,
        [Parameter(Mandatory)] [string]$Pattern,
        [Parameter(Mandatory)] [string]$FailureMessage
    )
    if ($Text -notmatch $Pattern) {
        throw $FailureMessage
    }
}

function Assert-DoesNotMatch {
    param(
        [Parameter(Mandatory)] [string]$Text,
        [Parameter(Mandatory)] [string]$Pattern,
        [Parameter(Mandatory)] [string]$FailureMessage
    )
    if ($Text -match $Pattern) {
        throw $FailureMessage
    }
}

$wrappers = @(
    @{
        Path = Join-Path $RepositoryRoot 'KswordARKDriver\src\features\kernel\driver_image_editor_list.c'
        Name = 'KswordARKDriverImageReadMemory'
    },
    @{
        Path = Join-Path $RepositoryRoot 'KswordARKDriver\src\platform\dyndata_fallback_resolver.c'
        Name = 'KswordARKDriverFallbackReadMemory'
    }
)

foreach ($wrapper in $wrappers) {
    $body = Get-CFunctionBody -Path $wrapper.Path -Name $wrapper.Name
    Assert-Matches `
        -Text $body `
        -Pattern '\bKswordARKRuntimeReadMemory\s*\(' `
        -FailureMessage "$($wrapper.Name) must delegate untrusted kernel reads to KswordARKRuntimeReadMemory."
    Assert-DoesNotMatch `
        -Text $body `
        -Pattern '\b(?:RtlCopyMemory|memcpy|memmove)\s*\(' `
        -FailureMessage "$($wrapper.Name) must not directly copy from an untrusted kernel address."
}

$runtimeReader = Join-Path $RepositoryRoot 'KswordARKDriver\src\platform\runtime_signature_scan.c'
$runtimeBody = Get-CFunctionBody -Path $runtimeReader -Name 'KswordARKRuntimeReadMemory'
Assert-Matches `
    -Text $runtimeBody `
    -Pattern '\bKeGetCurrentIrql\s*\(\s*\)\s*>\s*APC_LEVEL' `
    -FailureMessage 'KswordARKRuntimeReadMemory must reject calls above APC_LEVEL.'
Assert-Matches `
    -Text $runtimeBody `
    -Pattern '\bMmCopyMemory\s*\(' `
    -FailureMessage 'KswordARKRuntimeReadMemory must use MmCopyMemory for fault-contained reads.'
Assert-Matches `
    -Text $runtimeBody `
    -Pattern '\bbytesTransferred\s*==\s*Size' `
    -FailureMessage 'KswordARKRuntimeReadMemory must require the complete requested range.'
Assert-DoesNotMatch `
    -Text $runtimeBody `
    -Pattern '\b(?:RtlCopyMemory|memcpy|memmove)\s*\(' `
    -FailureMessage 'KswordARKRuntimeReadMemory must not regress to a direct copy.'

$bgpSource = Join-Path $RepositoryRoot 'KswordARKDriver\src\features\bugcheck\bugcheck_bgp.c'
$bgpViewBody = Get-CFunctionBody -Path $bgpSource -Name 'KswordARKBugcheckBgpInitializeImageView'
$bgpAddressBody = Get-CFunctionBody -Path $bgpSource -Name 'KswordARKBugcheckBgpAddressInSection'
$bgpScanBody = Get-CFunctionBody -Path $bgpSource -Name 'KswordARKBugcheckBgpScanSignatures'
$bgpDecodeBody = Get-CFunctionBody -Path $bgpSource -Name 'KswordARKBugcheckBgpDecodeRelativeCallAt'

Assert-Matches `
    -Text $bgpViewBody `
    -Pattern '\bKswordARKRuntimeReadMemory\s*\(' `
    -FailureMessage 'The BGP PE parser must read live kernel headers through the fault-contained reader.'
Assert-Matches `
    -Text $bgpAddressBody `
    -Pattern '\bIMAGE_SCN_MEM_DISCARDABLE\b' `
    -FailureMessage 'BGP target validation must reject discardable executable sections.'
Assert-Matches `
    -Text $bgpScanBody `
    -Pattern '\bIMAGE_SCN_MEM_DISCARDABLE\b' `
    -FailureMessage 'The BGP scanner must skip discardable executable sections before reading them.'
Assert-Matches `
    -Text $bgpScanBody `
    -Pattern '\bKswordARKRuntimeReadMemory\s*\(' `
    -FailureMessage 'The BGP scanner must snapshot each bounded window with the fault-contained reader.'
$discardableCheck = $bgpScanBody.IndexOf('IMAGE_SCN_MEM_DISCARDABLE', [StringComparison]::Ordinal)
$snapshotRead = $bgpScanBody.IndexOf('KswordARKRuntimeReadMemory(', [StringComparison]::Ordinal)
if ($discardableCheck -lt 0 -or $snapshotRead -lt 0 -or $discardableCheck -gt $snapshotRead) {
    throw 'The BGP scanner must reject discardable sections before its first snapshot read.'
}
Assert-Matches `
    -Text $bgpDecodeBody `
    -Pattern '\bMAXULONG_PTR\b' `
    -FailureMessage 'BGP relative-call decoding must reject pointer addition overflow.'
Assert-Matches `
    -Text $bgpDecodeBody `
    -Pattern '-\(LONGLONG\)displacement' `
    -FailureMessage 'BGP relative-call decoding must handle LONG_MIN without signed overflow.'

$bgpResolveBody = Get-CFunctionBody -Path $bgpSource -Name 'KswordARKBugcheckBgpResolveFunctions'
$bgpBeginDrawBody = Get-CFunctionBody -Path $bgpSource -Name 'KswordARKBugcheckBgpBeginDraw'
Assert-Matches `
    -Text $bgpResolveBody `
    -Pattern '\bInterlockedExchange\s*\(\s*&g_KswordArkBgp\.ResolvedSnapshotReady\s*,\s*1\s*\)' `
    -FailureMessage 'The BGP resolver must publish an explicit ready snapshot only after validation.'
Assert-Matches `
    -Text $bgpBeginDrawBody `
    -Pattern '\bResolvedSnapshotReady\b' `
    -FailureMessage 'The crash-time BGP draw path must reject an unpublished resolver snapshot.'

$bugcheckRuntime = Join-Path $RepositoryRoot 'KswordARKDriver\src\features\bugcheck\bugcheck_runtime.c'
$bugcheckCallbackBody = Get-CFunctionBody -Path $bugcheckRuntime -Name 'KswordARKBugcheckReasonCallback'
Assert-DoesNotMatch `
    -Text $bugcheckCallbackBody `
    -Pattern '\b(?:KswordARKBugcheckBgpScanSignatures|KswordARKBugcheckBgpResolveFunctions|KswordARKRuntimeReadMemory)\s*\(' `
    -FailureMessage 'The bugcheck callback must consume only prepared state and never scan or read the live kernel image.'

$driverEntry = Join-Path $RepositoryRoot 'KswordARKDriver\src\framework\driver_entry.c'
$driverEntryBody = Get-CFunctionBody -Path $driverEntry -Name 'DriverEntry'
$startCoreBody = Get-CFunctionBody -Path $driverEntry -Name 'KswordARKDriverStartCore'
$attachControllerBody = Get-CFunctionBody -Path $driverEntry -Name 'KswordARKDriverCoreAttachController'
$driverLifecycleCode = Get-CCodeText -Text (Get-Content -LiteralPath $driverEntry -Raw)
Assert-Matches `
    -Text $driverEntryBody `
    -Pattern '\bKswordARKDriverStartCore\s*\(' `
    -FailureMessage 'The legacy DriverEntry path must initialize the shared core before exposing IOCTLs.'
Assert-Matches `
    -Text $driverEntryBody `
    -Pattern 'g_KswordArkCore\.PnpProfile\s*\?\s*KsccEvtDeviceAdd\s*:' `
    -FailureMessage 'The PnP DriverEntry path must register the controller AddDevice lifecycle.'
Assert-Matches `
    -Text $attachControllerBody `
    -Pattern '\bKswordARKDriverStartCore\s*\(' `
    -FailureMessage 'The first PnP controller must initialize the same shared core as legacy DriverEntry.'
Assert-Matches `
    -Text (Get-CFunctionBody -Path (Join-Path $RepositoryRoot 'KswordARKDriver\src\features\storage_controller\driver.c') -Name 'KsccEvtDeviceAdd') `
    -Pattern '\bKswordARKDriverCoreAttachController\s*\(' `
    -FailureMessage 'The PnP AddDevice path must reach the shared core lifecycle.'
Assert-Matches `
    -Text $startCoreBody `
    -Pattern '(?s)\bKswordARKBugcheckControlInitialize\s*\(.*?\bKswordARKDriverPublishControlDevice\s*\(' `
    -FailureMessage 'Shared core startup must initialize the on-demand bugcheck controller before publishing IOCTLs.'
if ([regex]::Matches($driverLifecycleCode, '\bKswordARKBugcheckControlInitialize\s*\(').Count -ne 1) {
    throw 'The shared startup lifecycle must have exactly one on-demand bugcheck controller initialization site.'
}
$eagerBugcheckPattern = '\b(?:KswordARKBugcheckInitialize|KswordARKBugcheckBgp(?:Initialize|ScanSignatures|ResolveFunctions)|KeRegisterBugCheck(?:Reason)?Callback)\s*\('
Assert-DoesNotMatch `
    -Text $driverLifecycleCode `
    -Pattern $eagerBugcheckPattern `
    -FailureMessage 'DriverEntry and its core lifecycle helpers must not prepare BGP or register blue-screen callbacks before R3 requests installation.'

$bugcheckControl = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'KswordARKDriver\src\features\bugcheck\bugcheck_control.c') -Raw
$bugcheckControlInitializeBody = Get-CFunctionBody `
    -Path (Join-Path $RepositoryRoot 'KswordARKDriver\src\features\bugcheck\bugcheck_control.c') `
    -Name 'KswordARKBugcheckControlInitialize'
Assert-DoesNotMatch `
    -Text $bugcheckControlInitializeBody `
    -Pattern $eagerBugcheckPattern `
    -FailureMessage 'On-demand controller initialization must not prepare BGP or register blue-screen callbacks.'
Assert-DoesNotMatch `
    -Text $bugcheckControlInitializeBody `
    -Pattern '\bWdfWorkItemEnqueue\s*\(' `
    -FailureMessage 'On-demand controller initialization must not enqueue installation before an R3 request.'
$bugcheckConfigureBody = Get-CFunctionBody `
    -Path (Join-Path $RepositoryRoot 'KswordARKDriver\src\features\bugcheck\bugcheck_control.c') `
    -Name 'KswordARKBugcheckControlConfigure'
Assert-DoesNotMatch `
    -Text $bugcheckConfigureBody `
    -Pattern '\bKswordARKBugcheckInitialize\s*\(' `
    -FailureMessage 'The bugcheck installation IOCTL must enqueue work instead of synchronously running BGP preparation.'
Assert-Matches `
    -Text $bugcheckConfigureBody `
    -Pattern '\bWdfWorkItemEnqueue\s*\(' `
    -FailureMessage 'The bugcheck installation IOCTL must enqueue the bounded R0 preparation work item.'
Assert-DoesNotMatch `
    -Text $bugcheckControl `
    -Pattern 'attributes\.(?:ExecutionLevel|SynchronizationScope)\s*=' `
    -FailureMessage 'WDFWORKITEM attributes must inherit execution level and synchronization scope; KMDF rejects explicit values for this object type.'
Assert-Matches `
    -Text $bugcheckControl `
    -Pattern '(?s)KswordARKBugcheckControlUninitialize.*?\bWdfWorkItemFlush\s*\(' `
    -FailureMessage 'Driver unload must cancel and flush the bugcheck preparation work item before releasing resources.'
Assert-Matches `
    -Text $bgpScanBody `
    -Pattern '\bKswordARKBugcheckControlCheckAbort\s*\(' `
    -FailureMessage 'The bounded BGP signature scan must honor installation timeout and driver-unload cancellation.'

$bugcheckHeader = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'KswordARKDriver\include\ark\ark_bugcheck.h') -Raw
Assert-Matches `
    -Text $bugcheckHeader `
    -Pattern '(?s)#ifndef\s+KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ENABLED.*?#define\s+KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ENABLED\s+1' `
    -FailureMessage 'Driver-side bugcheck diagnostics must default to enabled for the development driver image.'

Write-Host 'safe-read regression passed: fallback and BGP scanners use fault-contained reads; discardable sections and crash-time rescans stay blocked.'
