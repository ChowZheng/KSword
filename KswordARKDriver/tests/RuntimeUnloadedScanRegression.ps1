[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$KernelImage,
    [Parameter(Mandatory)] [uint32]$ExpectedMmUnloadedDriversRva,
    [string]$RepositoryRoot = (Join-Path $PSScriptRoot '..\..')
)

$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$taskImage = (Resolve-Path -LiteralPath $KernelImage).Path
$taskOutput = Join-Path $taskRoot '.codex-build-logs\runtime-unloaded-scan-regression'
$taskVcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $taskVcvars)) { $taskVcvars = 'D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat' }
$taskKit = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
foreach ($taskRequired in @($taskVcvars, $taskKit)) {
    if (!(Test-Path -LiteralPath $taskRequired)) { throw "Required input missing: $taskRequired" }
}
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskMapped = Join-Path $taskOutput 'kernel-mapped.bin'
# The expected RVA is an independent test oracle. No PDB or profile is opened by the replay.
python -c 'import pefile,sys; p=pefile.PE(sys.argv[1]); b=p.get_memory_mapped_image(); open(sys.argv[2],"wb").write(b+bytes(p.OPTIONAL_HEADER.SizeOfImage-len(b)))' $taskImage $taskMapped
if ($LASTEXITCODE -ne 0) { throw 'Cannot map the supplied PE image.' }
$taskScanner = Join-Path $taskOutput 'runtime_signature_scan_replay.c'
$taskProduction = [IO.File]::ReadAllText((Join-Path $taskRoot 'KswordARKDriver\src\platform\runtime_signature_scan.c'))
$taskStart = $taskProduction.IndexOf('typedef struct _KSW_RUNTIME_ROUTINE_WORK')
if ($taskStart -lt 0) { throw 'Cannot locate production scanner definitions.' }
# Preserve all production function bodies, removing only the imported export resolver declaration.
[IO.File]::WriteAllText($taskScanner, $taskProduction.Substring($taskStart), [Text.UTF8Encoding]::new($false))
$taskResolver = Join-Path $taskOutput 'kernel_cache_unloaded_replay.c'
$taskProduction = [IO.File]::ReadAllText((Join-Path $taskRoot 'KswordARKDriver\src\platform\kernel_cache_fallback.c'))
$taskCut = [regex]::Match($taskProduction, 'static BOOLEAN\s+KswordARKKernelCacheAvlTableIsPlausible\(')
if (!$taskCut.Success) { throw 'Cannot locate production unloaded cache routines.' }
[IO.File]::WriteAllText($taskResolver, $taskProduction.Substring(0, $taskCut.Index), [Text.UTF8Encoding]::new($false))
$taskSource = Join-Path $taskOutput 'runtime_unloaded_scan_regression.c'
$taskExecutable = Join-Path $taskOutput 'runtime_unloaded_scan_regression.exe'
$taskCode = @'
#include <ntifs.h>
#include <ntimage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma warning(push)
#pragma warning(disable: 4324) // WDK/WDF framework header alignment.
#include "ark/ark_startup.h"
#include "kernel_cache_fallback.h"
#include "kernel_object_probe.h"
#include "pool_compat.h"
#include "runtime_signature_scan.h"
#pragma warning(pop)

#define RECORD_BASE 0xFFFF900000200000ULL
#define NAME_BASE 0xFFFF900000300000ULL
static UCHAR* image;
static ULONG imageSize;
static ULONG_PTR imageBase = 0xFFFF800000100000ULL;
static UCHAR fixtureRecords[6400];
static WCHAR driverName[] = L"fixture.sys";
static PVOID testSystemRangeStart = (PVOID)(ULONG_PTR)0xFFFF800000000000ULL;
static ULONG failures;

static BOOLEAN TestRead(const VOID* Address, VOID* Buffer, SIZE_T Size)
{
    ULONG_PTR address = (ULONG_PTR)Address;
    ULONG_PTR bases[3] = { imageBase, RECORD_BASE, NAME_BASE };
    const UCHAR* buffers[3] = { image, fixtureRecords, (const UCHAR*)driverName };
    SIZE_T sizes[3] = { imageSize, sizeof(fixtureRecords), sizeof(driverName) };
    ULONG index;
    for (index = 0UL; index < 3UL; ++index) {
        if (address >= bases[index] && address - bases[index] < sizes[index] &&
            Size <= sizes[index] - (SIZE_T)(address - bases[index])) {
            memcpy(Buffer, buffers[index] + address - bases[index], Size);
            return TRUE;
        }
    }
    return FALSE;
}

static NTSTATUS TestCopy(VOID* Buffer, MM_COPY_ADDRESS Source, SIZE_T Size, ULONG Flags, SIZE_T* Copied)
{
    UNREFERENCED_PARAMETER(Flags);
    *Copied = 0U;
    if (!TestRead(Source.VirtualAddress, Buffer, Size)) { return STATUS_PARTIAL_COPY; }
    *Copied = Size;
    return STATUS_SUCCESS;
}

static PVOID TestExport(PVOID Base, PCCH Name)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(image + dos->e_lfanew);
    IMAGE_EXPORT_DIRECTORY* exports = (IMAGE_EXPORT_DIRECTORY*)(image + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    ULONG index;
    UNREFERENCED_PARAMETER(Base);
    for (index = 0UL; index < exports->NumberOfNames; ++index) {
        ULONG nameRva = ((ULONG*)(image + exports->AddressOfNames))[index];
        USHORT ordinal = ((USHORT*)(image + exports->AddressOfNameOrdinals))[index];
        if (strcmp((const char*)image + nameRva, Name) == 0) {
            return (PVOID)(imageBase + ((ULONG*)(image + exports->AddressOfFunctions))[ordinal]);
        }
    }
    return NULL;
}

static KIRQL TestIrql(VOID) { return PASSIVE_LEVEL; }
static BOOLEAN TestSupported(VOID) { return TRUE; }
static VOID TestTime(PLARGE_INTEGER Time) { Time->QuadPart = 134000000000000000LL; }
static VOID TestInitString(PUNICODE_STRING String, PCWSTR Text)
{
    SIZE_T length = 0U;
    while (Text[length] != L'\0') { ++length; }
    String->Buffer = (PWSTR)Text;
    String->Length = (USHORT)(length * sizeof(WCHAR));
    String->MaximumLength = (USHORT)(String->Length + sizeof(WCHAR));
}
static BOOLEAN TestEqualString(PCUNICODE_STRING A, PCUNICODE_STRING B, BOOLEAN IgnoreCase)
{
    UNREFERENCED_PARAMETER(IgnoreCase);
    return A->Length == B->Length && memcmp(A->Buffer, B->Buffer, A->Length) == 0;
}
#undef KeGetCurrentIrql
#undef KeQuerySystemTime
#define KeGetCurrentIrql TestIrql
#define KeQuerySystemTime TestTime
#define MmCopyMemory TestCopy
#define MmSystemRangeStart testSystemRangeStart
#define RtlFindExportedRoutineByName TestExport
#define RtlInitUnicodeString TestInitString
#define RtlEqualUnicodeString TestEqualString
#define KswordArkStartupIsOsBuildSupported TestSupported
#include "@SCANNER@"
#include "@RESOLVER@"

static VOID Expect(const char* Name, BOOLEAN Passed)
{
    printf("%s %s\n", Passed ? "PASS" : "FAIL", Name);
    if (!Passed) { ++failures; }
}
static BOOLEAN Contains(const KSW_RUNTIME_DATA_REFERENCE* References, ULONG Count, ULONG Rva)
{
    ULONG index;
    for (index = 0UL; index < Count; ++index) {
        if (References[index].Address == imageBase + Rva) { return TRUE; }
    }
    return FALSE;
}
static VOID WriteRecord(SIZE_T Offset)
{
    UNICODE_STRING name;
    ULONGLONG start = 0xFFFFF80230000000ULL;
    ULONGLONG end = start + 0x4000ULL;
    LONGLONG time = 133999999900000000LL;
    TestInitString(&name, driverName); name.Buffer = (PWSTR)(ULONG_PTR)NAME_BASE;
    memcpy(fixtureRecords + Offset, &name, sizeof(name));
    memcpy(fixtureRecords + Offset + 16U, &start, sizeof(start));
    memcpy(fixtureRecords + Offset + 24U, &end, sizeof(end));
    memcpy(fixtureRecords + Offset + 32U, &time, sizeof(time));
}
int main(int Argc, char** Argv)
{
    FILE* file = NULL;
    long length;
    ULONG expectedRva;
    ULONG pass;
    IMAGE_NT_HEADERS64* nt;
    IMAGE_DATA_DIRECTORY saved;
    static PCSTR const anchors[] = { "MmUnloadSystemImage", "MmLoadSystemImage", "NtLoadDriver", "NtUnloadDriver" };
    KSW_RUNTIME_DATA_REFERENCE references[512];
    KSW_RUNTIME_IMAGE_VIEW view;
    KSW_RUNTIME_KERNEL_LAYOUT layout;
    ULONG count;
    ULONGLONG pointer = RECORD_BASE;
    if (Argc != 3 || fopen_s(&file, Argv[1], "rb") != 0) { return 2; }
    fseek(file, 0L, SEEK_END); length = ftell(file); fseek(file, 0L, SEEK_SET);
    if (length <= 0L || length > 0x4000000L) { fclose(file); return 2; }
    imageSize = (ULONG)length; image = (UCHAR*)malloc(imageSize);
    if (image == NULL || fread(image, 1U, imageSize, file) != imageSize) { fclose(file); free(image); return 2; }
    fclose(file); expectedRva = strtoul(Argv[2], NULL, 0);
    if (expectedRva >= imageSize || sizeof(pointer) > imageSize - expectedRva) { free(image); return 2; }
    for (pass = 0UL; pass < 2UL; ++pass) {
        RtlZeroMemory(&view, sizeof(view));
        Expect("initialize production PE view", KswordARKRuntimeInitializeImageView((PVOID)imageBase, imageSize, &view));
        count = KswordARKRuntimeCollectAnchoredDataReferences(&view, anchors, RTL_NUMBER_OF(anchors), 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
        printf("legacy refs=%lu targetFound=%u\n", count, Contains(references, count, expectedRva));
        count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, RTL_NUMBER_OF(anchors), 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
        Expect("PDB-free scan reaches independently verified global RVA", Contains(references, count, expectedRva));
        memcpy(image + expectedRva, &pointer, sizeof(pointer));
        RtlZeroMemory(fixtureRecords, sizeof(fixtureRecords)); WriteRecord(0U); WriteRecord(40U); WriteRecord(80U);
        RtlFillMemory(&layout, sizeof(layout), 0xFF);
        KswordARKKernelCacheResolveUnloadedDrivers(&view, references, count, &layout);
        Expect("production global and live layout discovery agree", layout.MmUnloadedDriversRva == (LONG)expectedRva &&
            layout.UldName == 0L && layout.UldStartAddress == 16L && layout.UldEndAddress == 24L && layout.UldCurrentTime == 32L && layout.UldTypeSize == 40L);
        imageBase += 0x10000000ULL;
    }
    KswordARKRuntimeInitializeImageView((PVOID)imageBase, imageSize, &view);
    nt = (IMAGE_NT_HEADERS64*)(image + ((IMAGE_DOS_HEADER*)image)->e_lfanew);
    saved = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = 0UL;
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, RTL_NUMBER_OF(anchors), 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("missing function table fails closed", count == 0UL);
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] = saved;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = MAXULONG;
    count = KswordARKRuntimeCollectFunctionDataReferences(&view, anchors, RTL_NUMBER_OF(anchors), 4UL, 0x800UL, references, RTL_NUMBER_OF(references));
    Expect("invalid function table extent fails closed", count == 0UL);
    free(image); printf("failures=%lu\n", failures);
    return failures != 0UL;
}
'@
$taskCode = $taskCode.Replace('@SCANNER@', $taskScanner.Replace('\', '/')).Replace('@RESOLVER@', $taskResolver.Replace('\', '/'))
[IO.File]::WriteAllText($taskSource, $taskCode, [Text.UTF8Encoding]::new($false))
$taskIncludes = @(
    (Join-Path $taskKit 'km'), (Join-Path $taskKit 'shared'), (Join-Path $taskKit 'ucrt'),
    'C:\Program Files (x86)\Windows Kits\10\Include\wdf\kmdf\1.15',
    (Join-Path $taskRoot 'KswordARKDriver\include'), (Join-Path $taskRoot 'KswordARKDriver'),
    (Join-Path $taskRoot 'KswordARKDriver\src\platform'), (Join-Path $taskRoot 'shared')
)
$taskCompile = @('call', ('"{0}"' -f $taskVcvars), '>', 'nul', '&&',
    'cl.exe', '/nologo', '/W4', '/WX', '/Od', '/Gy', '/utf-8', '/TC', '/D_AMD64_', '/DAMD64')
$taskCompile += $taskIncludes | ForEach-Object { '/I"{0}"' -f $_ }
$taskCompile += @(
    ('/Fo"{0}"' -f (Join-Path $taskOutput 'runtime_unloaded_scan_regression.obj')),
    ('/Fe"{0}"' -f $taskExecutable), ('"{0}"' -f $taskSource), '/link', '/OPT:REF'
)
& cmd.exe /d /s /c ($taskCompile -join ' ')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $taskExecutable $taskMapped $ExpectedMmUnloadedDriversRva
exit $LASTEXITCODE
