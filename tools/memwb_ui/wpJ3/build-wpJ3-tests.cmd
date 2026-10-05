@echo off
chcp 65001 >nul
rem ============================================================
rem build-wpJ3-tests.cmd
rem 作用：构建并运行 WP-J3（WorkbenchBaselineFeeder + HexCanvas 的
rem       settledPageStartsInRange 前置小改）离屏验证夹具
rem       （仿 tools\memwb_ui\wpE\build-wpE-tests.cmd 与 tools\memwb_ui\wpI\
rem       build-wpI-tests.cmd 的写法：vcvars64、Qt 路径、只链接需要的源文件、
rem       moc、/std:c++latest /W4 /WX）。
rem 用法：在任意目录执行 tools\memwb_ui\wpJ3\build-wpJ3-tests.cmd
rem 产物：MEMWB_OUT 指定的目录，默认 .codex-tmp\memwb-wpJ3（已被 gitignore 的
rem       .codex-tmp\ 规则覆盖）。
rem 说明：
rem  - 本机 MSVC 14.44 不识别 /std:c++23，会静默退回 C++14，所以使用 /std:c++latest。
rem  - HexCanvas 与 WorkbenchBaselineFeeder 都含 Q_OBJECT，先用 moc 生成 moc 文件
rem    再一起编译。
rem  - HexCanvas 需要 Qt6Widgets（QAbstractScrollArea）；本包的测试不做像素级
rem    截图/菜单验证，所以不链接 Qt6Svg、不部署图标/样式插件，只部署离屏平台插件
rem    （qoffscreen）让 QApplication 能在无显示环境下构造——比主夹具
rem    build-memwb-ui-tests.cmd 轻量。
rem  - 隔离：本脚本只编译链接 wpJ3 自己的文件 + HexCanvas 全家桶（含
rem    HexCanvasFormat）+ WorkbenchBaselineFeeder（本包产物）+ Core 的
rem    HexViewport/MemoryDiffOverlay/MemoryBaselineWindow 六个依赖源文件，不触碰
rem    tools\memwb_ui\ 下其它包的夹具或产物目录。
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
set "FIX=tools\memwb_ui\wpJ3"
rem MEMWB_OUT 可由调用方预先设置（写进命令行 set，不要共享环境变量），
rem 让并行跑多个工作包的夹具互不覆盖产物目录；默认 .codex-tmp\memwb-wpJ3。
if not defined MEMWB_OUT set "MEMWB_OUT=.codex-tmp\memwb-wpJ3"
set "OUT=%MEMWB_OUT%"
set "OBJ=%OUT%\obj"
set "MOC=%OUT%\moc"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%MOC%" mkdir "%MOC%"

rem ---- moc：HexCanvas 与 WorkbenchBaselineFeeder 都是 Q_OBJECT ----
"%QT%\bin\moc.exe" "%UI%\HexCanvas.h" -o "%MOC%\moc_HexCanvas.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\WorkbenchBaselineFeeder.h" -o "%MOC%\moc_WorkbenchBaselineFeeder.cpp"
if errorlevel 1 exit /b %errorlevel%

rem ---- 编译并链接 ----
if not defined CL set "CL=/MP"
rem 注：不在命令行传 WIN32_LEAN_AND_MEAN/NOMINMAX 的原因与 build-memwb-ui-tests.cmd
rem 一致——HexCanvas 自己的几个 .cpp 不含 Windows.h，这里仍传上以保持与主夹具相同
rem 的预处理环境，避免偶然依赖差异导致行为不一致。
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /Gy ^
  /DWIN32_LEAN_AND_MEAN /external:W0 /DNOMINMAX /DUNICODE /D_UNICODE ^
  /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB ^
  /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" ^
  "%FIX%\main.cpp" "%FIX%\wpJ3_common.cpp" ^
  "%FIX%\wpJ3_tests.SettledPages.cpp" "%FIX%\wpJ3_tests.Debounce.cpp" "%FIX%\wpJ3_tests.Decision.cpp" "%FIX%\wpJ3_tests.Lifecycle.cpp" "%FIX%\wpJ3_tests.Regression.cpp" "%FIX%\wpJ3_tests.Review2.cpp" ^
  "%UI%\HexCanvas.cpp" "%UI%\HexCanvas.Scroll.cpp" "%UI%\HexCanvas.Layout.cpp" "%UI%\HexCanvas.Paint.cpp" "%UI%\HexCanvas.Input.cpp" "%UI%\HexCanvas.Edit.cpp" "%UI%\HexCanvas.Menu.cpp" "%UI%\HexCanvasFormat.cpp" ^
  "%UI%\WorkbenchBaselineFeeder.cpp" ^
  "%MOC%\moc_HexCanvas.cpp" "%MOC%\moc_WorkbenchBaselineFeeder.cpp" ^
  "%CORE%\HexViewport.cpp" "%CORE%\HexViewport.Cache.cpp" "%CORE%\HexViewport.Selection.cpp" ^
  "%CORE%\MemoryDiffOverlay.cpp" "%CORE%\MemoryDiffOverlay.Patches.cpp" "%CORE%\MemoryBaselineWindow.cpp" ^
  /Fo"%OBJ%\\" /Fe"%OUT%\wpJ3_tests.exe" ^
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
"%OUT%\wpJ3_tests.exe" %*
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
