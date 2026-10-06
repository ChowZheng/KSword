@echo off
chcp 65001 >nul
rem ============================================================
rem build-wpJ1-tests.cmd
rem 作用：构建并运行 WP-J1（WorkbenchShared 进程级单例）离屏验证夹具。
rem 本包只链接需要的源文件：WorkbenchShared 本身 + 它直接持有的三个共享对象
rem （AddressBookStore/AddressBookModel/Int3Controller）+ 它们各自的 Core 依赖，
rem 不链接 HexCanvas / AddressBookPanel / Int3PatchPanel 等界面层（WorkbenchShared
rem 完全不依赖它们）。
rem
rem 与其它工作包的夹具不同：本包不构造 QApplication，只构造 QCoreApplication——
rem WorkbenchShared 不展示任何窗口，不需要平台插件（qoffscreen.dll 等只有
rem QGuiApplication/QApplication 才会加载），因此不需要部署 plugins\platforms 目录，
rem 只需要把 Qt6Core/Qt6Gui/Qt6Widgets 三个 DLL 放到产物目录（Int3Controller.cpp
rem 引用了 QtWidgets 的类型，链接期需要这三个库，运行期本包从不构造任何 QWidget，
rem 所以不触发真正的 GUI 初始化）。
rem
rem 单例跨场景污染的处理方式（见 wpJ1_main.cpp 顶部注释）：本可执行文件每次只跑
rem "--scenario <名字>" 指定的那一个场景，由本脚本按下面固定的场景名单依次起一次
rem 全新的进程（从而拿到一个全新、从未 Configure 过的 WorkbenchShared 单例），把
rem 各次的 stdout 拼接进同一份日志，最后用 findstr 统计 "CHECK " 总行数与
rem "CHECK FAIL" 行数，拼出与其它工作包同样格式的汇总行。下面的场景名单必须与四个
rem 场景文件的调度链条合起来保持一致：wpJ1_scenarios.cpp 的 RunScenario() → 认不出
rem 就转 wpJ1_scenarios_review.cpp 的 RunReviewScenario()（wave3 第一轮审核补测场景
rem 与本包当时自补的新场景）→ 认不出就转 wpJ1_scenarios_r2.cpp 的 RunR2Scenario()
rem （wave3 第二轮审核 review2-wpJ1.md 并入的补测场景）→ 认不出就转
rem wpJ1_scenarios_r2b.cpp 的 RunR2bScenario()（本包这一轮自己新补的场景，四个文件
rem 各自不超过单文件 800 行的仓库规范）——任何一边不同步时，最终会落到调度链条
rem 最末端的"未识别场景"分支，对应产生一条 CHECK FAIL，不会被静默吞掉。
rem
rem 用法：在任意目录执行 tools\memwb_ui\wpJ1\build-wpJ1-tests.cmd
rem       可用环境变量 MEMWB_OUT 覆盖产物目录（默认 .codex-tmp\memwb-wpJ1）。
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
set "FIX=tools\memwb_ui\wpJ1"
rem MEMWB_OUT 可由调用方预先设置，让多个验证互不覆盖产物目录；默认 .codex-tmp\memwb-wpJ1。
if not defined MEMWB_OUT set "MEMWB_OUT=.codex-tmp\memwb-wpJ1"
set "OUT=%MEMWB_OUT%"
set "OBJ=%OUT%\obj"
set "OBJ2=%OUT%\obj2"
set "MOC=%OUT%\moc"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%OBJ2%" mkdir "%OBJ2%"
if not exist "%MOC%" mkdir "%MOC%"
if not exist "%OUT%\scratch" mkdir "%OUT%\scratch"

rem ---- moc：四个 Q_OBJECT 类（WorkbenchShared 本身 + 它持有的三个共享对象）----
"%QT%\bin\moc.exe" "%UI%\WorkbenchShared.h" -o "%MOC%\moc_WorkbenchShared.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\AddressBookStore.h" -o "%MOC%\moc_AddressBookStore.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\AddressBookModel.h" -o "%MOC%\moc_AddressBookModel.cpp"
if errorlevel 1 exit /b %errorlevel%
"%QT%\bin\moc.exe" "%UI%\Int3Controller.h" -o "%MOC%\moc_Int3Controller.cpp"
if errorlevel 1 exit /b %errorlevel%

rem ---- 第二遍：LanguageManager（AddressBookModel 经 ks::i18n::sourceText/displayText
rem      查语言包；它不是本包新写的代码，主程序用 Level3 编译它，这里仿 wpE 的做法单独
rem      一次 /W3 不开 /WX，不让它既有的警告挡住本包自己代码的强校验）----
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W3 /O2 /DWIN32_LEAN_AND_MEAN /external:W0 /DNOMINMAX /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB ^
  /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" ^
  /c "%APP%\Internationalization\LanguageManager.cpp" ^
  /Fo"%OBJ2%\\"
if errorlevel 1 exit /b %errorlevel%

rem ---- 第一遍：本包新代码 + 复用的既有实现，/W4 /WX 干净 ----
if not defined CL set "CL=/MP"
cl /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /Gy /DWIN32_LEAN_AND_MEAN /external:W0 /DNOMINMAX /DUNICODE /D_UNICODE /DQT_CORE_LIB /DQT_GUI_LIB /DQT_WIDGETS_LIB /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" ^
  "%FIX%\wpJ1_main.cpp" "%FIX%\wpJ1_common.cpp" "%FIX%\wpJ1_scenarios.cpp" "%FIX%\wpJ1_scenarios_review.cpp" "%FIX%\wpJ1_scenarios_r2.cpp" "%FIX%\wpJ1_scenarios_r2b.cpp" ^
  "%UI%\WorkbenchShared.cpp" "%UI%\AddressBookStore.cpp" "%UI%\AddressBookModel.cpp" "%UI%\AddressBookModel.StoreSync.cpp" "%UI%\Int3Controller.cpp" ^
  "%MOC%\moc_WorkbenchShared.cpp" "%MOC%\moc_AddressBookStore.cpp" "%MOC%\moc_AddressBookModel.cpp" "%MOC%\moc_Int3Controller.cpp" ^
  "%CORE%\MemoryAddressBook.cpp" "%CORE%\MemoryAddressBook.Serialize.cpp" ^
  "shared\evidence\PointerChain.cpp" ^
  "%CORE%\Int3PatchLedger.cpp" "%CORE%\MemoryPatchByteStore.cpp" "%CORE%\MemoryTargetSession.cpp" "%CORE%\MemoryWritePolicy.cpp" ^
  /Fo"%OBJ%\\" /Fe"%OUT%\wpJ1_tests.exe" ^
  /link /OPT:REF /LIBPATH:"%QT%\lib" Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib user32.lib ^
  "%OBJ2%\LanguageManager.obj"
if errorlevel 1 exit /b %errorlevel%

rem ---- 部署 DLL（只需要 Core/Gui/Widgets 三个；不需要任何平台插件，见文件头注释）----
for %%D in (Qt6Core Qt6Gui Qt6Widgets) do copy /y "%QT%\bin\%%D.dll" "%OUT%\" >nul

rem ---- 按固定场景名单逐个起一次全新进程，拼接输出 ----
set "PATH=%OUT%;%QT%\bin;%PATH%"
set "MEMWB_OUT=%OUT%"
set "LOG=%OUT%\scenarios.log"
if exist "%LOG%" del "%LOG%"
rem 场景名单分五段拼接（原始 11 个 + wave3 第一轮审核补测 Gap/新增 9+1 个 + Defect
rem 7 个 + wave3 第二轮审核（review2-wpJ1.md）并入的 11+1 个 + 本包这一轮自己新补的
rem 5 个），五段分别在 wpJ1_scenarios.cpp/wpJ1_scenarios_review.cpp/
rem wpJ1_scenarios_r2.cpp/wpJ1_scenarios_r2b.cpp 的 RunScenario/RunReviewScenario/
rem RunR2Scenario/RunR2bScenario 里有对应分支——Defect_* 与 R2Defect* 按任务书要求
rem "DEFECT 类用例修复后转绿并并入默认运行"，不再单独用环境变量区分一组"预期失败"
rem 的场景。
set "SCENARIOS_ORIGINAL=DegradedBeforeConfigure ConfigureRejectsInvalidBackendsThenSucceeds ConfigureOnceOnlyKeepsOriginal RealPathPersistenceAndShutdownIdempotent ShutdownNeverConfiguredIsSafe AuditSinkDefaultsToNullWhenFactoryOmitted AuditSinkRealFactoryUsedAndStable PortFactoriesProduceFreshInstancesEachCall KernelPortOptionalStaysNullWhenOmitted WritePolicyIsStableAcrossCalls Int3UsesInjectedFactoryAfterConfigure"
set "SCENARIOS_GAP=GapInvalidConfigureLeavesNoResidue GapRepeatConfigureKeepsFactoriesAndAudit GapCorruptBookDoesNotFailConfigure GapQuitSignalFlushes GapIsConfiguredFalseAfterLazyAccess GapWritePolicySurvivesConfigureAndShutdown GapAddressBookTableIsFirstAccess GapConfigureRejectionWarnsAboutOrder GapBothEarlyAccessRejectsConfigureButInt3StillUsable InstanceBeforeAppHooksOnConfigure"
set "SCENARIOS_DEFECT=DefectShutdownZeroEditsDoesNotRewriteBook DefectShutdownAfterFailedLoadDoesNotCreateMainFile DefectInt3OnlyEarlyTouchSurvivesConfigure DefectConfigureAfterLazyAccessDoesNotDangle DefectPreConfigureAuditRefReachesRealSink DefectThrowingAuditFactoryLeavesUnconfigured DefectEarlyShutdownDoesNotDisableQuitFlush"
set "SCENARIOS_R2=R2DuplicateConfigureNoSpuriousWarning R2Int3ForwardsArgumentsInt3First R2Int3ForwardsArgumentsConfigureFirst R2AuditRecordContentForwarded R2RejectedByOrderLeavesNoResidue R2ThrowingAuditFactoryLeavesNoResidue R2ThrowingAuditFactoryDoesNotTouchDisk R2ShutdownFlushesEditsAfterEarlierFlush R2ShutdownKeepsSharedObjectsAlive R2ExitTimeDestructorFlush R2EmptyPathConfigureAccepted R2DefectReentrantAuditFactoryDoesNotDangle"
set "SCENARIOS_R2B=R2DefectReentrantConfigureCallRejected R2DefectRejectedConfigureDoesNotHookQuit R2DefectAppRecreateReHooks R2ConfigureRejectedReasonTracksEachCase R2ConfigureRejectedReasonAccessedBeforeConfigure"
set "SCENARIOS=%SCENARIOS_ORIGINAL% %SCENARIOS_GAP% %SCENARIOS_DEFECT% %SCENARIOS_R2% %SCENARIOS_R2B%"
set "ANYNONZERO=0"
rem 注意：子进程异常终止（例如解引用空指针、调用空 std::function）在 Windows 上
rem 产生的退出码通常是 0x8xxxxxxx/0xCxxxxxxx 一类的大数，cmd 把 ERRORLEVEL 当有符号
rem 32 位整数看待时这类值是负数；"if errorlevel 1" 做的是">= 1"的有符号比较，对负数
rem 恒为假，单独用它会漏判崩溃（已实测：CreateServices 的一个变异体因为这个漏洞
rem 被错误地判定为 SURVIVED，修复后能正确判定为暴露，见 impl-wpJ1.md"变异验证"一节）。
rem 这里用"errorlevel 1"（命中正的非零）与"not errorlevel 0"（命中负的非零）两条
rem 配合，覆盖全部非零退出码；两者都是 if 的专用语法，在括号块里仍然按每次命令执行后
rem 动态查询，不是被当成普通变量在解析时冻结一次。
for %%S in (%SCENARIOS%) do (
  echo ==== %%S ==== >> "%LOG%"
  "%OUT%\wpJ1_tests.exe" --scenario %%S >> "%LOG%" 2>&1
  if errorlevel 1 (
    set "ANYNONZERO=1"
  ) else if not errorlevel 0 (
    set "ANYNONZERO=1"
  )
)

type "%LOG%"

rem ---- 统计总检查数与失败数，拼出与其它工作包同样格式的汇总行 ----
rem 2026-10 wave3 修复期间实测发现：旧写法 "for /f %%N in ('findstr ... ^| find /c /v
rem ""') do ..." 用的是"命令替换"形式——cmd.exe 要为这条管道另起一个隐藏子 cmd.exe
rem 去跑 findstr/find 并把结果重定向进一个内部临时文件再读回来。在 vcvars64.bat 铺好
rem 的巨大环境变量（尤其是很长的 PATH）之上、又经由完全脱离控制台的方式（本机某些
rem 自动化调用链会这样起本脚本）调用时，这条隐藏子 cmd.exe 偶发卡死且不断派生新的
rem find.exe 子进程（已用 Get-CimInstance 实测抓到），一次要吃掉数百秒甚至被外层
rem 超时杀掉，而这与 CHECK 结果本身完全无关——同一份 scenarios.log 用 PowerShell
rem Select-String 直接读，几十毫秒就能读完。改成"for /f ... in (文件)"直接读文件这种
rem 形式：这是纯粹的文件遍历，不会为了取值另起任何子进程，彻底绕开这个隐藏子 cmd.exe
rem 的卡死点。
set "TOTAL=0"
set "FAILS=0"
findstr /r /c:"^CHECK " "%LOG%" > "%OUT%\checks-only.txt" 2>nul
for /f "usebackq delims=" %%N in ("%OUT%\checks-only.txt") do set /a TOTAL+=1
findstr /c:"CHECK FAIL" "%LOG%" > "%OUT%\fails-only.txt" 2>nul
for /f "usebackq delims=" %%N in ("%OUT%\fails-only.txt") do set /a FAILS+=1
echo wpJ1_tests: %TOTAL% checks, %FAILS% failures

popd
if not "%FAILS%"=="0" exit /b 1
if "%ANYNONZERO%"=="1" exit /b 1
exit /b 0
