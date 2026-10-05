@echo off
chcp 65001 >nul
rem ============================================================
rem build-wpJ2-quickcheck.cmd
rem 作用：只编译（不链接）WorkbenchPageProvider.cpp / .Pool.cpp，快速抓语法/
rem       类型错误，不构建完整夹具（完整夹具见 build-wpJ2-tests.cmd）。
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
set "UI=%APP%\UI\MemoryWorkbench"
if not defined MEMWB_OUT set "MEMWB_OUT=.codex-tmp\memwb-wpJ2-quickcheck"
set "OUT=%MEMWB_OUT%"
set "OBJ=%OUT%\obj"
if not exist "%OBJ%" mkdir "%OBJ%"

if not defined CL set "CL=/MP"
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /DWIN32_LEAN_AND_MEAN /external:W0 /DNOMINMAX /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB ^
  /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" ^
  /c "%UI%\WorkbenchPageProvider.cpp" "%UI%\WorkbenchPageProvider.Pool.cpp" ^
  /Fo"%OBJ%\\"
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
