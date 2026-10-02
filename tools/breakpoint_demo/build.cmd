@echo off
setlocal
set "VS64=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VS64%" exit /b 1
call "%VS64%" >nul
if errorlevel 1 exit /b 1
if /i not "%VSCMD_ARG_HOST_ARCH%"=="x64" exit /b 1
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" exit /b 1
cd /d "%~dp0"
if not exist "out" mkdir "out"
cl /nologo /W4 /Od /Zi /MT /Fe:out\BreakpointDemo-x64.exe /Fo:out\breakpoint_demo.obj /Fd:out\compiler.pdb breakpoint_demo.c /link /DEBUG /INCREMENTAL:NO /PDB:out\BreakpointDemo-x64.pdb Psapi.lib
if errorlevel 1 exit /b 1
cl /nologo /W4 /Od /Zi /MT /Fe:out\BreakpointDemoGUI-x64.exe /Fo:out\breakpoint_demo_gui.obj /Fd:out\compiler-gui.pdb breakpoint_demo_gui.c /link /SUBSYSTEM:WINDOWS /DEBUG /INCREMENTAL:NO /PDB:out\BreakpointDemoGUI-x64.pdb Psapi.lib User32.lib
exit /b %errorlevel%
