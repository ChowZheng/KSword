@echo off
chcp 65001 >nul
rem ============================================================
rem build-wpJ4-tests.cmd
rem 作用：构建并运行 WP-J4（WorkbenchWriteController + WorkbenchUndoCoordinator）
rem       离屏验证夹具。仿 tools\memwb_ui\wpG\build-wpG-tests.cmd（需要 Qt6Widgets，
rem       因为真实 WorkbenchConfirmations 用到 QMessageBox）与 wpI 的写法
rem       （WorkbenchTarget 是 QObject，需要 moc + 六个 Core 依赖源文件）。
rem 用法：在任意目录执行 tools\memwb_ui\wpJ4\build-wpJ4-tests.cmd
rem 产物：MEMWB_OUT 指定的目录，默认 .codex-tmp\memwb-wpJ4\（已被 gitignore 的
rem       .codex-tmp\ 规则覆盖）。
rem 隔离：只编译链接 wpJ4 自己的文件 + WorkbenchWriteController.*（本包产物）+
rem       WorkbenchTarget.*/WorkbenchConfirmations.cpp/WorkbenchMessages.cpp（依赖）+
rem       Core 的十五个依赖源文件，不触碰 tools\memwb_ui\ 下其它包的夹具或产物
rem       目录，也不修改它们。
rem 修复记录：原脚本漏链接 Internationalization\LanguageManager.cpp
rem       （WorkbenchMessages.cpp 经 ks::i18n::LanguageManager 查语言包），导致
rem       LNK2019 找不到 LanguageManager::instance/sourceText——这是本次装配层
rem       修复过程中为了能跑通夹具而补上的脚本缺口，仿 wpE/wpG/wpH/wpJ1 几个
rem       夹具同样"第二遍 /W3 单独编译"的做法，不改动 LanguageManager 本身。
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
set "FIX=tools\memwb_ui\wpJ4"
set "SUPPORT=KswordARKLightTests"
set "WPI=tools\memwb_ui\wpI"
rem MEMWB_OUT 可由调用方预先设置（写进命令行 set，不要共享环境变量），默认
rem .codex-tmp\memwb-wpJ4。
if not defined MEMWB_OUT set "MEMWB_OUT=.codex-tmp\memwb-wpJ4"
set "OUT=%MEMWB_OUT%"
set "OBJ=%OUT%\obj"
set "OBJ2=%OUT%\obj2"
set "MOC=%OUT%\moc"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%OBJ2%" mkdir "%OBJ2%"
if not exist "%MOC%" mkdir "%MOC%"

rem ---- moc：WorkbenchWriteController（本包）与 WorkbenchTarget（依赖）都是
rem      QObject，需要生成信号槛元代码 ----
"%QT%\bin\moc.exe" "%UI%\WorkbenchWriteController.h" -o "%MOC%\moc_WorkbenchWriteController.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\WorkbenchTarget.h" -o "%MOC%\moc_WorkbenchTarget.cpp"
if errorlevel 1 exit /b %errorlevel%

rem ---- 第二遍：LanguageManager（WorkbenchMessages.cpp 经 ks::i18n::LanguageManager::
rem      instance()/sourceText 查语言包拼通道/范围描述文案；它不是本包新写的代码，
rem      主程序用 Level3 编译它，这里仿 wpE/wpG/wpH/wpJ1 几个夹具同样的做法单独一次
rem      /W3 不开 /WX，不让它既有的警告挡住本包自己代码的强校验。修复前本脚本漏了
rem      这一步，导致链接期 LNK2019 找不到 LanguageManager::instance/sourceText，
rem      是夹具脚本自身的缺口，不是 WorkbenchWriteController 的问题） ----
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W3 /O2 /DWIN32_LEAN_AND_MEAN /external:W0 /DNOMINMAX /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB ^
  /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" ^
  /c "%APP%\Internationalization\LanguageManager.cpp" ^
  /Fo"%OBJ2%\\"
if errorlevel 1 exit /b %errorlevel%

rem ---- 编译并链接（需要 Qt6Widgets：真实 WorkbenchConfirmations.cpp 用到
rem      QMessageBox/QCheckBox/QPushButton，夹具注入假 IConfirmPrompter 避免真的
rem      弹出模态框，但源码本身仍然要链接 QtWidgets） ----
if not defined CL set "CL=/MP"
rem 注：不在命令行传 WIN32_LEAN_AND_MEAN/NOMINMAX——含 Windows.h 的几个文件自己
rem 在 #include <Windows.h> 之前 #define 这两个宏；命令行再传一遍会和文件内的
rem #define 撞成 C4005（宏重定义），/WX 下变成编译错误。
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /Gy ^
  /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB /external:W0 ^
  /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" ^
  /I"%SUPPORT%" /I"%WPI%" ^
  "%FIX%\wpJ4_main.cpp" "%FIX%\wpJ4_common.cpp" ^
  "%FIX%\wpJ4_tests.Lifecycle.cpp" "%FIX%\wpJ4_tests.Commit.cpp" "%FIX%\wpJ4_tests.PendingStage.cpp" ^
  "%FIX%\wpJ4_tests.Undo.cpp" "%FIX%\wpJ4_tests.Confirmations.cpp" ^
  "%UI%\WorkbenchWriteController.cpp" "%UI%\WorkbenchWriteController.PendingStage.cpp" "%UI%\WorkbenchWriteController.Undo.cpp" ^
  "%UI%\WorkbenchTarget.cpp" "%UI%\WorkbenchTarget.Anchor.cpp" "%UI%\WorkbenchTarget.Modules.cpp" ^
  "%UI%\WorkbenchConfirmations.cpp" "%UI%\WorkbenchMessages.cpp" ^
  "%WPI%\memwb_wpI_common.cpp" ^
  "%MOC%\moc_WorkbenchWriteController.cpp" "%MOC%\moc_WorkbenchTarget.cpp" ^
  "%CORE%\MemoryTargetSession.cpp" "%CORE%\MemoryTargetTracker.cpp" "%CORE%\MemoryModuleDirectory.cpp" ^
  "%CORE%\MemoryProcessMatch.cpp" "%CORE%\MemoryAddressExpr.cpp" "%CORE%\SessionAddressResolver.cpp" ^
  "%CORE%\MemoryChannelGate.cpp" "%CORE%\MemoryWritePolicy.cpp" ^
  "%CORE%\MemoryDiffOverlay.cpp" "%CORE%\MemoryDiffOverlay.Patches.cpp" ^
  "%CORE%\MemoryWriteTransaction.cpp" "%CORE%\MemoryWriteTransaction.Commit.cpp" ^
  "%CORE%\MemoryIoByteStore.cpp" "%CORE%\MemoryKernelMutation.cpp" "%CORE%\MemoryEditJournal.cpp" ^
  /Fo"%OBJ%\\" /Fe"%OUT%\wpJ4_tests.exe" ^
  /link /OPT:REF /LIBPATH:"%QT%\lib" Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib user32.lib advapi32.lib ^
  "%OBJ2%\LanguageManager.obj"
if errorlevel 1 exit /b %errorlevel%

rem ---- 部署 DLL 与插件（离屏平台；WorkbenchConfirmations 不需要图标/样式插件，
rem      但 QApplication 在 Windows 上仍需要 platforms 插件才能构造） ----
for %%D in (Qt6Core Qt6Gui Qt6Widgets) do copy /y "%QT%\bin\%%D.dll" "%OUT%\" >nul
if not exist "%OUT%\plugins\platforms" mkdir "%OUT%\plugins\platforms"
copy /y "%QT%\plugins\platforms\qoffscreen.dll" "%OUT%\plugins\platforms\" >nul

rem ---- 运行 ----
set "PATH=%OUT%;%QT%\bin;%PATH%"
set "QT_PLUGIN_PATH=%OUT%\plugins"
set "QT_QPA_PLATFORM=offscreen"
"%OUT%\wpJ4_tests.exe" %*
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
