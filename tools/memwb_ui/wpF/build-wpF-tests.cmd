@echo off
chcp 65001 >nul
rem ============================================================
rem build-wpF-tests.cmd
rem 作用：构建并运行 WP-F（int3 补丁：Int3Controller + Int3PatchPanel）离屏验证夹具。
rem 用法：在任意目录执行 tools\memwb_ui\wpF\build-wpF-tests.cmd [--shots <目录>]
rem       额外参数原样传给 memwb_wpF_tests.exe。
rem 产物：.codex-tmp\memwb-wpF\（已被 gitignore 的 .codex-tmp 覆盖）。
rem 说明：
rem  - 本机 MSVC 14.44 不识别 /std:c++23，会静默退回 C++14，所以使用 /std:c++latest。
rem  - Int3Controller / Int3PatchPanel 含 Q_OBJECT，先用 moc 生成 moc 文件再一起编译。
rem  - 只链接本工作包需要的源文件：Core 的 Int3PatchLedger / MemoryPatchByteStore /
rem    MemoryTargetSession（int3 账本与它的真实存取器落点），UI 的 Int3Controller /
rem    Int3PatchPanel；不链接 HexCanvas 等本包不需要的大型依赖。
rem  - 夹具内嵌精简 qrc（本包用到的 6 个图标别名），需要 Qt6Svg 与 svg 图标引擎/图片格式插件。
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
set "FIX=tools\memwb_ui\wpF"
rem MEMWB_OUT 可由调用方预先设置，让多个验证互不覆盖产物目录；默认 .codex-tmp\memwb-wpF。
if not defined MEMWB_OUT set "MEMWB_OUT=.codex-tmp\memwb-wpF"
set "OUT=%MEMWB_OUT%"
set "OBJ=%OUT%\obj"
set "OBJ2=%OUT%\obj2"
set "MOC=%OUT%\moc"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%OBJ2%" mkdir "%OBJ2%"
if not exist "%MOC%" mkdir "%MOC%"
if not exist "%OUT%\shots" mkdir "%OUT%\shots"

rem ---- moc / rcc：Int3Controller / Int3PatchPanel 的信号槽与夹具图标资源 ----
"%QT%\bin\moc.exe" "%UI%\Int3Controller.h" -o "%MOC%\moc_Int3Controller.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\Int3PatchPanel.h" -o "%MOC%\moc_Int3PatchPanel.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\rcc.exe" "%FIX%\wpF_icons.qrc" -name wpF_icons -o "%MOC%\qrc_wpF_icons.cpp"
if errorlevel 1 exit /b %errorlevel%

rem ---- LanguageManager: compile existing dependency separately without /WX ----
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W3 /WX- /O2 /DWIN32_LEAN_AND_MEAN /external:W0 /DNOMINMAX /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB ^
  /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" ^
  /c "%APP%\Internationalization\LanguageManager.cpp" ^
  /Fo"%OBJ2%\\"
if errorlevel 1 exit /b %errorlevel%

rem ---- 编译并链接 ----
if not defined CL set "CL=/MP"
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /Gy /DWIN32_LEAN_AND_MEAN /external:W0 /DNOMINMAX /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB /DQT_TESTLIB_LIB /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" /external:I"%QT%\include\QtTest" ^
  "%FIX%\wpF_main.cpp" "%FIX%\wpF_tests_common.cpp" "%FIX%\wpF_tests_controller.cpp" "%FIX%\wpF_tests_panel.cpp" ^
  "%UI%\Int3Controller.cpp" "%UI%\Int3PatchPanel.cpp" ^
  "%CORE%\Int3PatchLedger.cpp" "%CORE%\MemoryPatchByteStore.cpp" "%CORE%\MemoryTargetSession.cpp" ^
  "%MOC%\moc_Int3Controller.cpp" "%MOC%\moc_Int3PatchPanel.cpp" "%MOC%\qrc_wpF_icons.cpp" ^
  /Fo"%OBJ%\\" /Fe"%OUT%\memwb_wpF_tests.exe" ^
  /link /OPT:REF /LIBPATH:"%QT%\lib" Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib Qt6Test.lib user32.lib advapi32.lib ^
  "%OBJ2%\LanguageManager.obj"
if errorlevel 1 exit /b %errorlevel%

rem ---- 部署 DLL 与插件（离屏平台、SVG 图标引擎与图片格式） ----
for %%D in (Qt6Core Qt6Gui Qt6Widgets Qt6Test Qt6Svg) do copy /y "%QT%\bin\%%D.dll" "%OUT%\" >nul
if not exist "%OUT%\plugins\platforms" mkdir "%OUT%\plugins\platforms"
if not exist "%OUT%\plugins\iconengines" mkdir "%OUT%\plugins\iconengines"
if not exist "%OUT%\plugins\imageformats" mkdir "%OUT%\plugins\imageformats"
if not exist "%OUT%\plugins\styles" mkdir "%OUT%\plugins\styles"
copy /y "%QT%\plugins\platforms\qoffscreen.dll" "%OUT%\plugins\platforms\" >nul
copy /y "%QT%\plugins\platforms\qwindows.dll" "%OUT%\plugins\platforms\" >nul
copy /y "%QT%\plugins\iconengines\qsvgicon.dll" "%OUT%\plugins\iconengines\" >nul
copy /y "%QT%\plugins\imageformats\qsvg.dll" "%OUT%\plugins\imageformats\" >nul
copy /y "%QT%\plugins\styles\*.dll" "%OUT%\plugins\styles\" >nul 2>nul

rem ---- 运行 ----
set "PATH=%OUT%;%QT%\bin;%PATH%"
set "QT_PLUGIN_PATH=%OUT%\plugins"
set "QT_QPA_PLATFORM=offscreen"
"%OUT%\memwb_wpF_tests.exe" --shots "%OUT%\shots" %*
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
