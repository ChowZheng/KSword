@echo off
chcp 65001 >nul
rem ============================================================
rem build-wpJ0-check.cmd
rem 作用：WP-J0（内存工作台装配层接口冻结）头文件编译检查。只编译
rem tools\memwb_ui\wpJ0\headers_compile_check.cpp（它只 #include 六个新冻结的
rem 装配层头文件及其直接依赖），/W4 /WX 下零警告即视为"接口至少自洽"的最低证据。
rem 只做编译，不链接、不运行——装配层头文件本身声明的对象需要真实或假的工厂/
rem 服务实现才能构造，那是后续实现工作包的职责，不在本次接口冻结范围内。
rem 仿 tools\memwb_ui\wpE\build-wpE-tests.cmd 的写法：vcvars64、Qt 路径、/W4 /WX。
rem 用法：在任意目录执行 tools\memwb_ui\wpJ0\build-wpJ0-check.cmd
rem       可用环境变量 MEMWB_OUT 覆盖产物目录（默认 .codex-tmp\memwb-wpJ0），
rem       便于与其它工作包的夹具并行跑而不互相覆盖。
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

set "FIX=tools\memwb_ui\wpJ0"
rem MEMWB_OUT 可由调用方预先设置，让多个验证互不覆盖产物目录；默认 .codex-tmp\memwb-wpJ0。
if not defined MEMWB_OUT set "MEMWB_OUT=.codex-tmp\memwb-wpJ0"
set "OUT=%MEMWB_OUT%"
set "OBJ=%OUT%\obj"
if not exist "%OBJ%" mkdir "%OBJ%"

rem ---- 只编译，不链接：/c 产出 .obj 即可，headers_compile_check.cpp 的 main 为空 ----
if not defined CL set "CL=/MP"
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /DWIN32_LEAN_AND_MEAN /external:W0 /DNOMINMAX /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB ^
  /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" ^
  /c "%FIX%\headers_compile_check.cpp" ^
  /Fo"%OBJ%\\"
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
