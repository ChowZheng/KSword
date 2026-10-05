@echo off
chcp 65001 >nul
rem ============================================================
rem build-wpK1-tests.cmd
rem Purpose: build and run the WP-K1 fixture. No Qt link, no driver, no Dock: it links only
rem            Ksword5.1\Ksword5.1\MemoryDock\WorkbenchServicesMapping.cpp   (pure functions)
rem            Ksword5.1\Ksword5.1\MemoryDock\WorkbenchServicesAudit.cpp     (pure functions)
rem            Ksword5.1\Ksword5.1\MemoryDock\WorkbenchServicesProbe.cpp     (pure state machine)
rem            Ksword5.1\Ksword5.1\MemoryDock\MemoryDock.WorkbenchServices.Audit.cpp (production audit sink)
rem            Ksword5.1\Ksword5.1\ksword\log\log.cpp                        (real project log, Win32 + std only)
rem            shared\evidence\memory_workbench\MemoryTargetSession.cpp      (IdentityKey / Validate)
rem          The audit sink includes Framework.h, so Qt and project include paths are needed at
rem          COMPILE time (headers only; nothing from Qt is linked, hence /DQT_NO_VERSION_TAGGING).
rem          The other host files (MemoryDock.WorkbenchServices.cpp/.Impl/.Gate/.Disasm) need the real
rem          driver client and UI components and are only syntax-checked by build-wpK1-check.cmd.
rem Usage:   tools\memwb_ui\wpK1\build-wpK1-tests.cmd
rem Output:  %MEMWB_OUT% (default .codex-tmp\memwb-wpK1). The last stdout line is exactly
rem          "wpK1_tests: N checks, M failures". Exit code 0 means all checks passed.
rem Flags:   /std:c++latest /W4 /WX like the other memwb fixtures.
rem Notes:   ASCII only on purpose (UTF-8 plus LF batch files are parsed badly by cmd).
rem ============================================================
setlocal
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
set "CORE=shared\evidence\memory_workbench"
set "FIX=tools\memwb_ui\wpK1"
if not defined MEMWB_OUT set "MEMWB_OUT=.codex-tmp\memwb-wpK1"
set "OUT=%MEMWB_OUT%"
set "OBJ=%OUT%\obj"
if not exist "%OBJ%" mkdir "%OBJ%"

if not defined CL set "CL=/MP"
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /external:W0 ^
  /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DZYDIS_STATIC_BUILD /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB /DQT_NO_VERSION_TAGGING ^
  /I"%APP%" /I"third_party\zstd" /I"third_party\zydis" ^
  /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" /external:I"%QT%\include\QtNetwork" /external:I"%QT%\include\QtSvg" ^
  "%FIX%\wpK1_main.cpp" "%FIX%\wpK1_common.cpp" ^
  "%FIX%\wpK1_tests.MappingModules.cpp" "%FIX%\wpK1_tests.MappingPointer.cpp" "%FIX%\wpK1_tests.MappingMisc.cpp" ^
  "%FIX%\wpK1_tests.AuditClassify.cpp" "%FIX%\wpK1_tests.AuditFormat.cpp" "%FIX%\wpK1_tests.AuditRegistry.cpp" ^
  "%FIX%\wpK1_tests.AuditSink.cpp" ^
  "%FIX%\wpK1_tests.ProbeTracker.cpp" "%FIX%\wpK1_tests.ProbeDecision.cpp" ^
  "%DOCK%\WorkbenchServicesMapping.cpp" "%DOCK%\WorkbenchServicesAudit.cpp" "%DOCK%\WorkbenchServicesProbe.cpp" ^
  "%DOCK%\MemoryDock.WorkbenchServices.Audit.cpp" "%APP%\ksword\log\log.cpp" ^
  "%CORE%\MemoryTargetSession.cpp" ^
  /Fo"%OBJ%\\" /Fe"%OUT%\wpK1_tests.exe" ^
  /link /OPT:REF Ole32.lib
set "COMPILE=%errorlevel%"
if not "%COMPILE%"=="0" (
  popd
  exit /b %COMPILE%
)

"%OUT%\wpK1_tests.exe" %*
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
