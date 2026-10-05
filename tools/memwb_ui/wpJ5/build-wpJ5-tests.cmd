@echo off
chcp 65001 >nul
rem ============================================================
rem build-wpJ5-tests.cmd
rem 作用：构建并运行 WP-J5（WorkbenchHexPane）离屏验证夹具。仿
rem       tools\memwb_ui\wpJ2\build-wpJ2-tests.cmd（HexCanvas 是 QObject，需要
rem       moc；WorkbenchPageProvider/WorkbenchTarget 的真实依赖源文件）与
rem       tools\memwb_ui\wpJ4\build-wpJ4-tests.cmd（WorkbenchWriteController 的
rem       真实依赖源文件；复用 wpI 的 FakeWorkbenchServices/AttachFakeProcess，
rem       只读引用不修改）的写法。本包额外需要 HexInspectorPanel/HexFindBar
rem       家族（真实 HexCanvas + 真实阶段 1 的类 + 本包自己的假端口/假服务）与
rem       Qt6Test（QTest::keyClick/qWaitForWindowActive、QSignalSpy，用于真实
rem       按键驱动 Esc/F3 的回归）。
rem 用法：在任意目录执行 tools\memwb_ui\wpJ5\build-wpJ5-tests.cmd [夹具参数...]
rem 产物：MEMWB_OUT 指定的目录，默认 .codex-tmp\memwb-wpJ5（已被 gitignore 的
rem       .codex-tmp\ 规则覆盖）；截图落在 MEMWB_OUT\shots\ 下。
rem 隔离：只编译链接本包自己的夹具文件 + WorkbenchHexPane(.Panels).cpp（本包
rem       产物）+ WorkbenchPageProvider(.Pool)/WorkbenchBaselineFeeder/
rem       WorkbenchWriteController(.PendingStage/.Undo)/WorkbenchTarget(.Anchor/
rem       .Modules)（阶段 1 依赖）+ HexCanvas/HexInspectorPanel/HexFindBar 三个
rem       既有家族 + 对应的 Core 层依赖，不触碰 tools\memwb_ui\ 下其它包的夹具
rem       或产物目录，也不修改 wpI 的公共设施。
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
set "FIX=tools\memwb_ui\wpJ5"
set "WPI=tools\memwb_ui\wpI"
rem MEMWB_OUT 可由调用方预先设置（写进命令行 set，不要共享环境变量），让并行
rem 跑多个工作包的夹具互不覆盖产物目录；默认 .codex-tmp\memwb-wpJ5。本程序自己
rem 也会在运行期读取这个环境变量来决定截图落盘位置（见 wpJ5_tests.Visual.cpp）。
if not defined MEMWB_OUT set "MEMWB_OUT=.codex-tmp\memwb-wpJ5"
set "OUT=%MEMWB_OUT%"
set "OBJ=%OUT%\obj"
set "MOC=%OUT%\moc"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%MOC%" mkdir "%MOC%"
if not exist "%OUT%\shots" mkdir "%OUT%\shots"

rem ---- moc：本包与依赖里全部 Q_OBJECT 类 ----
"%QT%\bin\moc.exe" "%UI%\HexCanvas.h" -o "%MOC%\moc_HexCanvas.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\HexInspectorPanel.h" -o "%MOC%\moc_HexInspectorPanel.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\HexInspectorRowView.h" -o "%MOC%\moc_HexInspectorRowView.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\HexFindBar.h" -o "%MOC%\moc_HexFindBar.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\HexViewWidgets.h" -o "%MOC%\moc_HexViewWidgets.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\WorkbenchTarget.h" -o "%MOC%\moc_WorkbenchTarget.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\WorkbenchPageProvider.h" -o "%MOC%\moc_WorkbenchPageProvider.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\WorkbenchBaselineFeeder.h" -o "%MOC%\moc_WorkbenchBaselineFeeder.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\WorkbenchWriteController.h" -o "%MOC%\moc_WorkbenchWriteController.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\WorkbenchHexPane.h" -o "%MOC%\moc_WorkbenchHexPane.cpp"
if errorlevel 1 exit /b %errorlevel%

rem ---- 编译并链接 ----
if not defined CL set "CL=/MP"
rem 注：不在命令行传 WIN32_LEAN_AND_MEAN/NOMINMAX——含 Windows.h 的文件
rem（WorkbenchTarget.Anchor.cpp）自己在 #include <Windows.h> 之前 #define 这两
rem 个宏；命令行再传一遍会和文件内的 #define 撞成 C4005，在 /WX 下变成编译
rem 错误（wpJ2/wpJ4 已经踩过这个坑，这里照抄规避方式）。
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /Gy ^
  /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB /DQT_TESTLIB_LIB /external:W0 ^
  /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" /external:I"%QT%\include\QtTest" ^
  "%FIX%\wpJ5_main.cpp" "%FIX%\wpJ5_common.cpp" ^
  "%FIX%\wpJ5_tests.Wiring.cpp" "%FIX%\wpJ5_tests.Baseline.cpp" "%FIX%\wpJ5_tests.Write.cpp" ^
  "%FIX%\wpJ5_tests.Find.cpp" "%FIX%\wpJ5_tests.Navigation.cpp" "%FIX%\wpJ5_tests.Visual.cpp" ^
  "%UI%\WorkbenchHexPane.cpp" "%UI%\WorkbenchHexPane.Panels.cpp" ^
  "%UI%\WorkbenchPageProvider.cpp" "%UI%\WorkbenchPageProvider.Pool.cpp" ^
  "%UI%\WorkbenchBaselineFeeder.cpp" ^
  "%UI%\WorkbenchWriteController.cpp" "%UI%\WorkbenchWriteController.PendingStage.cpp" "%UI%\WorkbenchWriteController.Undo.cpp" ^
  "%UI%\WorkbenchTarget.cpp" "%UI%\WorkbenchTarget.Anchor.cpp" "%UI%\WorkbenchTarget.Modules.cpp" ^
  "%UI%\HexCanvas.cpp" "%UI%\HexCanvas.Scroll.cpp" "%UI%\HexCanvas.Layout.cpp" "%UI%\HexCanvas.Paint.cpp" "%UI%\HexCanvas.Input.cpp" "%UI%\HexCanvas.Edit.cpp" "%UI%\HexCanvas.Menu.cpp" "%UI%\HexCanvasFormat.cpp" ^
  "%UI%\HexInspectorPanel.cpp" "%UI%\HexInspectorPanel.Rows.cpp" "%UI%\HexInspectorPanel.Edit.cpp" "%UI%\HexInspectorPanel.Menu.cpp" "%UI%\HexInspectorRowView.cpp" "%UI%\HexInspectorRowView.Paint.cpp" "%UI%\HexInspectorWidgets.cpp" ^
  "%UI%\HexFindBar.cpp" "%UI%\HexFindSearch.cpp" "%UI%\HexViewWidgets.cpp" "%UI%\HexViewWidgets.Text.cpp" "%UI%\HexViewFormat.cpp" ^
  "%WPI%\memwb_wpI_common.cpp" ^
  "%MOC%\moc_HexCanvas.cpp" "%MOC%\moc_HexInspectorPanel.cpp" "%MOC%\moc_HexInspectorRowView.cpp" "%MOC%\moc_HexFindBar.cpp" "%MOC%\moc_HexViewWidgets.cpp" ^
  "%MOC%\moc_WorkbenchTarget.cpp" "%MOC%\moc_WorkbenchPageProvider.cpp" "%MOC%\moc_WorkbenchBaselineFeeder.cpp" "%MOC%\moc_WorkbenchWriteController.cpp" "%MOC%\moc_WorkbenchHexPane.cpp" ^
  "%CORE%\HexViewport.cpp" "%CORE%\HexViewport.Cache.cpp" "%CORE%\HexViewport.Selection.cpp" ^
  "%CORE%\MemoryDiffOverlay.cpp" "%CORE%\MemoryDiffOverlay.Patches.cpp" "%CORE%\MemoryTargetSession.cpp" ^
  "%CORE%\MemoryValueDecode.cpp" "%CORE%\MemoryAddressExpr.cpp" "%CORE%\MemoryByteSearch.cpp" ^
  "%CORE%\MemoryTargetTracker.cpp" "%CORE%\MemoryModuleDirectory.cpp" "%CORE%\MemoryProcessMatch.cpp" "%CORE%\SessionAddressResolver.cpp" ^
  "%CORE%\MemoryChannelGate.cpp" "%CORE%\MemoryPageReader.cpp" ^
  "%CORE%\MemoryWriteTransaction.cpp" "%CORE%\MemoryWriteTransaction.Commit.cpp" "%CORE%\MemoryIoByteStore.cpp" "%CORE%\MemoryKernelMutation.cpp" "%CORE%\MemoryEditJournal.cpp" ^
  "%CORE%\MemoryBaselineWindow.cpp" ^
  /Fo"%OBJ%\\" /Fe"%OUT%\wpJ5_tests.exe" ^
  /link /OPT:REF /LIBPATH:"%QT%\lib" Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib Qt6Test.lib user32.lib advapi32.lib
if errorlevel 1 exit /b %errorlevel%

rem ---- 部署运行期依赖的 Qt DLL 与离屏平台插件 ----
for %%D in (Qt6Core Qt6Gui Qt6Widgets Qt6Test) do copy /y "%QT%\bin\%%D.dll" "%OUT%\" >nul
if not exist "%OUT%\plugins\platforms" mkdir "%OUT%\plugins\platforms"
copy /y "%QT%\plugins\platforms\qoffscreen.dll" "%OUT%\plugins\platforms\" >nul

rem ---- 运行 ----
set "PATH=%OUT%;%QT%\bin;%PATH%"
set "QT_PLUGIN_PATH=%OUT%\plugins"
set "QT_QPA_PLATFORM=offscreen"
"%OUT%\wpJ5_tests.exe" %*
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
