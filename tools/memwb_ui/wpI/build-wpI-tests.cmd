@echo off
chcp 65001 >nul
rem ============================================================
rem build-wpI-tests.cmd
rem 作用：构建并运行 WP-I（WorkbenchTarget / 目标与导航接入）离屏验证夹具
rem       （仿 tools\memwb_ui\build-memwb-ui-tests.cmd，但本包只依赖 Qt6Core——
rem       WorkbenchTarget 是 QObject 而不是 QWidget，用不到 Widgets/Gui/离屏平台
rem       插件，构建更轻更快）。
rem 用法：在任意目录执行 tools\memwb_ui\wpI\build-wpI-tests.cmd
rem 产物：MEMWB_OUT 指定的目录，默认 .codex-tmp\memwb-wpI\（已被 gitignore 的
rem       .codex-tmp\ 规则覆盖）。
rem 隔离：本脚本只编译链接 wpI 自己的文件 + WorkbenchTarget.*（本包产物）+
rem       Core 的六个依赖源文件（MemoryTargetSession/Tracker、MemoryModuleDirectory、
rem       MemoryProcessMatch、MemoryAddressExpr、SessionAddressResolver），不触碰
rem       tools\memwb_ui\ 下其它包的夹具或产物目录。
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
if not exist "%QT%\lib\Qt6Core.lib" set "QT=D:\Software\Qt\6.9.3\msvc2022_64"
if not exist "%QT%\lib\Qt6Core.lib" exit /b 2

set "APP=Ksword5.1\Ksword5.1"
set "CORE=shared\evidence\memory_workbench"
set "UI=%APP%\UI\MemoryWorkbench"
set "FIX=tools\memwb_ui\wpI"
rem MEMWB_OUT 可由调用方预先设置（写进命令行 set，不要共享环境变量），
rem 让并行跑多个工作包的夹具互不覆盖产物目录；默认 .codex-tmp\memwb-wpI。
if not defined MEMWB_OUT set "MEMWB_OUT=.codex-tmp\memwb-wpI"
set "OUT=%MEMWB_OUT%"
set "OBJ=%OUT%\obj"
set "MOC=%OUT%\moc"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%MOC%" mkdir "%MOC%"

rem ---- moc：WorkbenchTarget 是 QObject，需要生成信号槅元代码 ----
"%QT%\bin\moc.exe" "%UI%\WorkbenchTarget.h" -o "%MOC%\moc_WorkbenchTarget.cpp"
if errorlevel 1 exit /b %errorlevel%

rem ---- 编译并链接（只用 Qt6Core：本包的类都是 QObject，不是 QWidget） ----
if not defined CL set "CL=/MP"
rem 注：不在命令行传 WIN32_LEAN_AND_MEAN/NOMINMAX——含 Windows.h 的几个文件自己
rem 在 #include <Windows.h> 之前 #define 这两个宏；命令行再传一遍会和文件内的
rem #define 撞成 C4005（宏重定义），/WX 下变成编译错误。
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /Gy ^
  /DUNICODE /D_UNICODE /DQT_CORE_LIB /external:W0 ^
  /external:I"%QT%\include" /external:I"%QT%\include\QtCore" ^
  "%FIX%\main.cpp" "%FIX%\memwb_wpI_common.cpp" ^
  "%FIX%\memwb_wpI_tests.Hooks.cpp" "%FIX%\memwb_wpI_tests.Guard.cpp" "%FIX%\memwb_wpI_tests.Ddma.cpp" ^
  "%FIX%\memwb_wpI_tests.Revision.cpp" ^
  "%FIX%\memwb_wpI_tests.Modules.cpp" "%FIX%\memwb_wpI_tests.Evaluate.cpp" "%FIX%\memwb_wpI_tests.Anchor.cpp" ^
  "%FIX%\memwb_wpI_tests.HandleLifecycle.cpp" "%FIX%\memwb_wpI_tests.Stress.cpp" ^
  "%FIX%\memwb_wpI_tests.Accessors.cpp" ^
  "%FIX%\memwb_wpI_tests.Extra.cpp" "%FIX%\memwb_wpI_tests.Fix2.cpp" ^
  "%UI%\WorkbenchTarget.cpp" "%UI%\WorkbenchTarget.Anchor.cpp" "%UI%\WorkbenchTarget.Modules.cpp" ^
  "%MOC%\moc_WorkbenchTarget.cpp" ^
  "%CORE%\MemoryTargetSession.cpp" "%CORE%\MemoryTargetTracker.cpp" "%CORE%\MemoryModuleDirectory.cpp" ^
  "%CORE%\MemoryProcessMatch.cpp" "%CORE%\MemoryAddressExpr.cpp" "%CORE%\SessionAddressResolver.cpp" ^
  /Fo"%OBJ%\\" /Fe"%OUT%\memwb_wpI_tests.exe" ^
  /link /OPT:REF /LIBPATH:"%QT%\lib" Qt6Core.lib user32.lib advapi32.lib
if errorlevel 1 exit /b %errorlevel%

rem ---- 部署运行期依赖的 Qt DLL ----
copy /y "%QT%\bin\Qt6Core.dll" "%OUT%\" >nul

rem ---- 运行 ----
set "PATH=%OUT%;%QT%\bin;%PATH%"
"%OUT%\memwb_wpI_tests.exe" %*
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
