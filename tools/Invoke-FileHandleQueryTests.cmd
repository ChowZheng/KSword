@echo off
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" exit /b 2
call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%
set "OUT=%CD%\.codex-build-logs\file-handle-query-tests"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /permissive- /utf-8 /EHsc /MD /W4 /WX /DNOMINMAX tools\file_handle_query_timeout_test.cpp /Fo"%OUT%\query.obj" /Fe"%OUT%\query.exe"
if errorlevel 1 exit /b %errorlevel%
"%OUT%\query.exe"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++17 /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /Gy /DNOMINMAX tools\file_handle_scan_fallback_test.cpp Ksword5.1\Ksword5.1\ksword\file\file_handle_tools.cpp /Fo"%OUT%\\" /Fe"%OUT%\fallback.exe" /link /OPT:REF advapi32.lib
if errorlevel 1 exit /b %errorlevel%
"%OUT%\fallback.exe"
if errorlevel 1 exit /b %errorlevel%
"%OUT%\fallback.exe" --r3-live
exit /b %errorlevel%
