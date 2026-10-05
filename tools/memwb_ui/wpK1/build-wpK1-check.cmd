@echo off
chcp 65001 >nul
rem ============================================================
rem build-wpK1-check.cmd
rem Purpose: syntax-only compile check (cl /Zs, no obj, no link) of the WP-K1 host files
rem          in MemoryDock\ against the main program include paths and defines, plus the header
rem          contract probe wpK1_header_contract.cpp (binds the 7 frozen free functions of
rem          MemoryDock.WorkbenchServices.h to the std::function types of the MemoryWorkbenchView
rem          injection points).
rem          The main program (Ksword5.1.vcxproj) is NOT built here; this only proves the
rem          new .cpp files parse and type-check against the real headers (Framework.h, Qt,
rem          ArkDriverClient, KernelThreadAuditTab, ...).
rem Usage:   tools\memwb_ui\wpK1\build-wpK1-check.cmd [warning-level-flag]
rem          Default warning flag is /W4; pass /W3 to match the main project exactly.
rem          Warnings are errors (/WX).
rem Output:  one "[OK]" or "[FAIL]" line per file and a final line
rem          "wpK1_check: N files, M failures". Exit code 0 only when M is 0.
rem Notes:   This file is ASCII only on purpose (LF/CRLF batch parsing quirks with UTF-8).
rem ============================================================
setlocal EnableDelayedExpansion
set "REPO=%~dp0..\..\.."
pushd "%REPO%"
if errorlevel 1 exit /b 2

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" exit /b 2
call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%

set "QT=%CD%\.deps\Qt\6.9.3\msvc2022_64"
if not exist "%QT%\lib\Qt6Widgets.lib" set "QT=D:\Software\Qt\6.9.3\msvc2022_64"
if not exist "%QT%\lib\Qt6Widgets.lib" exit /b 2

set "APP=Ksword5.1\Ksword5.1"
set "DOCK=%APP%\MemoryDock"
set "FIX=tools\memwb_ui\wpK1"
set "WARN=%~1"
if "%WARN%"=="" set "WARN=/W4"

set "FILES=%DOCK%\MemoryDock.WorkbenchServices.cpp %DOCK%\MemoryDock.WorkbenchServices.Impl.cpp %DOCK%\MemoryDock.WorkbenchServices.Gate.cpp %DOCK%\MemoryDock.WorkbenchServices.Audit.cpp %DOCK%\MemoryDock.WorkbenchServices.Disasm.cpp %DOCK%\WorkbenchServicesMapping.cpp %DOCK%\WorkbenchServicesAudit.cpp %DOCK%\WorkbenchServicesProbe.cpp %FIX%\wpK1_header_contract.cpp"

set /a TOTAL=0
set /a FAILS=0
for %%F in (%FILES%) do (
  set /a TOTAL+=1
  cl /nologo /Zs /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD %WARN% /WX /external:W0 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DZYDIS_STATIC_BUILD /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB ^
    /I"%APP%" /I"third_party\zstd" /I"third_party\zydis" ^
    /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" /external:I"%QT%\include\QtNetwork" /external:I"%QT%\include\QtSvg" ^
    "%%F" > "%TEMP%\wpK1_check_%%~nxF.log" 2>&1
  if errorlevel 1 (
    set /a FAILS+=1
    echo [FAIL] %%~nxF
    type "%TEMP%\wpK1_check_%%~nxF.log"
  ) else (
    echo [OK] %%~nxF
  )
)
echo wpK1_check: !TOTAL! files, !FAILS! failures
popd
if !FAILS! neq 0 exit /b 1
exit /b 0
