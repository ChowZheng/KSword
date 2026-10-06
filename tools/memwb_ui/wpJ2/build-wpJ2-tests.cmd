@echo off
chcp 65001 >nul
rem ============================================================
rem build-wpJ2-tests.cmd
rem 作用：构建并运行 WP-J2（WorkbenchPageProvider）离屏验证夹具（仿
rem       tools\memwb_ui\wpI\build-wpI-tests.cmd 与
rem       tools\memwb_ui\build-memwb-ui-tests.cmd 的写法：vcvars64、Qt 路径、
rem       只链接需要的源文件、moc、/std:c++latest /W4 /WX）。
rem 用法：在任意目录执行 tools\memwb_ui\wpJ2\build-wpJ2-tests.cmd
rem 产物：MEMWB_OUT 指定的目录，默认 .codex-tmp\memwb-wpJ2（已被 gitignore 的
rem       .codex-tmp\ 规则覆盖）。
rem 隔离：本脚本只编译链接 wpJ2 自己的夹具文件 + WorkbenchPageProvider.*/
rem       WorkbenchTarget.*（本包与 WP-I 的产物，装配层六个类里只这两个要
rem       真正跑）+ 真实 HexCanvas 家族 + 对应的 Core 层依赖，不触碰
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
if not exist "%QT%\lib\Qt6Widgets.lib" set "QT=D:\Software\Qt\6.9.3\msvc2022_64"
if not exist "%QT%\lib\Qt6Widgets.lib" exit /b 2

set "APP=Ksword5.1\Ksword5.1"
set "CORE=shared\evidence\memory_workbench"
set "UI=%APP%\UI\MemoryWorkbench"
set "FIX=tools\memwb_ui\wpJ2"
rem MEMWB_OUT 可由调用方预先设置（写进命令行 set，不要共享环境变量），
rem 让并行跑多个工作包的夹具互不覆盖产物目录；默认 .codex-tmp\memwb-wpJ2。
if not defined MEMWB_OUT set "MEMWB_OUT=.codex-tmp\memwb-wpJ2"
set "OUT=%MEMWB_OUT%"
set "OBJ=%OUT%\obj"
set "MOC=%OUT%\moc"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%MOC%" mkdir "%MOC%"

rem ---- moc：HexCanvas / WorkbenchTarget / WorkbenchPageProvider 都是 QObject ----
"%QT%\bin\moc.exe" "%UI%\HexCanvas.h" -o "%MOC%\moc_HexCanvas.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\WorkbenchTarget.h" -o "%MOC%\moc_WorkbenchTarget.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\WorkbenchPageProvider.h" -o "%MOC%\moc_WorkbenchPageProvider.cpp"
if errorlevel 1 exit /b %errorlevel%

rem ---- 编译并链接 ----
if not defined CL set "CL=/MP"
rem 注：不在命令行传 WIN32_LEAN_AND_MEAN/NOMINMAX——WorkbenchTarget.Anchor.cpp
rem 含 Windows.h 的文件自己在 #include <Windows.h> 之前 #define 这两个宏；
rem 命令行再传一遍会和文件内的 #define 撞成 C4005，在 /WX 下变成编译错误。
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /Gy ^
  /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB /external:W0 ^
  /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" ^
  "%FIX%\wpJ2_main.cpp" "%FIX%\wpJ2_common.cpp" ^
  "%FIX%\wpJ2_tests.Gate.cpp" "%FIX%\wpJ2_tests.Delivery.cpp" "%FIX%\wpJ2_tests.Stale.cpp" ^
  "%FIX%\wpJ2_tests.ChannelFailed.cpp" "%FIX%\wpJ2_tests.Latch.cpp" "%FIX%\wpJ2_tests.Serialize.cpp" ^
  "%FIX%\wpJ2_tests.Lifecycle.cpp" "%FIX%\wpJ2_tests.Supp.cpp" "%FIX%\wpJ2_tests.Decision2.cpp" ^
  "%FIX%\wpJ2_tests.Supp2.cpp" ^
  "%UI%\WorkbenchPageProvider.cpp" "%UI%\WorkbenchPageProvider.Pool.cpp" ^
  "%UI%\WorkbenchTarget.cpp" "%UI%\WorkbenchTarget.Anchor.cpp" "%UI%\WorkbenchTarget.Modules.cpp" ^
  "%UI%\HexCanvas.cpp" "%UI%\HexCanvas.Scroll.cpp" "%UI%\HexCanvas.Layout.cpp" "%UI%\HexCanvas.Paint.cpp" "%UI%\HexCanvas.Input.cpp" "%UI%\HexCanvas.Edit.cpp" "%UI%\HexCanvas.Menu.cpp" "%UI%\HexCanvasFormat.cpp" ^
  "%MOC%\moc_HexCanvas.cpp" "%MOC%\moc_WorkbenchTarget.cpp" "%MOC%\moc_WorkbenchPageProvider.cpp" ^
  "%CORE%\HexViewport.cpp" "%CORE%\HexViewport.Cache.cpp" "%CORE%\HexViewport.Selection.cpp" ^
  "%CORE%\MemoryDiffOverlay.cpp" "%CORE%\MemoryDiffOverlay.Patches.cpp" "%CORE%\MemoryTargetSession.cpp" ^
  "%CORE%\MemoryTargetTracker.cpp" "%CORE%\MemoryModuleDirectory.cpp" "%CORE%\MemoryProcessMatch.cpp" ^
  "%CORE%\MemoryAddressExpr.cpp" "%CORE%\SessionAddressResolver.cpp" ^
  "%CORE%\MemoryChannelGate.cpp" "%CORE%\MemoryPageReader.cpp" ^
  /Fo"%OBJ%\\" /Fe"%OUT%\wpJ2_tests.exe" ^
  /link /OPT:REF /LIBPATH:"%QT%\lib" Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib user32.lib advapi32.lib
if errorlevel 1 exit /b %errorlevel%

rem ---- 部署运行期依赖的 Qt DLL 与离屏平台插件 ----
for %%D in (Qt6Core Qt6Gui Qt6Widgets) do copy /y "%QT%\bin\%%D.dll" "%OUT%\" >nul
if not exist "%OUT%\plugins\platforms" mkdir "%OUT%\plugins\platforms"
copy /y "%QT%\plugins\platforms\qoffscreen.dll" "%OUT%\plugins\platforms\" >nul

rem ---- 运行 ----
set "PATH=%OUT%;%QT%\bin;%PATH%"
set "QT_PLUGIN_PATH=%OUT%\plugins"
set "QT_QPA_PLATFORM=offscreen"
"%OUT%\wpJ2_tests.exe" %*
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
