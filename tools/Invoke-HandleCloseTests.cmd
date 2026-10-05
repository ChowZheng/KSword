@echo off
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" exit /b 2
call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%
set "OUT=%CD%\.codex-build-logs\handle-close-tests"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /permissive- /utf-8 /EHsc /MD /W4 /WX /DNOMINMAX tools\handle_close_client_test.cpp Ksword5.1\Ksword5.1\ArkDriverClient\ArkDriverHandle.cpp /Fo"%OUT%\\" /Fe"%OUT%\client.exe"
if errorlevel 1 exit /b %errorlevel%
"%OUT%\client.exe"
if errorlevel 1 exit /b %errorlevel%
python tools\generate_handle_close_kernel_test.py "%OUT%\kernel.cpp"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++17 /permissive- /utf-8 /EHsc /MD /W4 /WX /DNOMINMAX "%OUT%\kernel.cpp" /Fo"%OUT%\kernel.obj" /Fe"%OUT%\kernel.exe"
if errorlevel 1 exit /b %errorlevel%
"%OUT%\kernel.exe"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++17 /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /Gy /DNOMINMAX tools\handle_close_r3_test.cpp Ksword5.1\Ksword5.1\ksword\file\file_handle_tools.cpp /Fo"%OUT%\\" /Fe"%OUT%\r3.exe" /link /OPT:REF advapi32.lib
if errorlevel 1 exit /b %errorlevel%
"%OUT%\r3.exe"
exit /b %errorlevel%
