@echo off
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" exit /b 2
call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%
set "QT=%CD%\.deps\Qt\6.9.3\msvc2022_64"
if not exist "%QT%\lib\Qt6Widgets.lib" set "QT=D:\Software\Qt\6.9.3\msvc2022_64"
if not exist "%QT%\lib\Qt6Widgets.lib" exit /b 2
set "APP=Ksword5.1\Ksword5.1"
set "OUT=tools\hvm_lab\artifacts\ui-tests"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /Zc:__cplusplus /permissive- /utf-8 /EHsc /FI"%CD%\tools\hvm_lab\hvm_ui_test_access.h" /MD /W4 /WX /O2 /Gy /DWIN32_LEAN_AND_MEAN /external:W0 /DNOMINMAX /DUNICODE /D_UNICODE /Ithird_party/qt_advanced_docking_system/src /external:I"%QT%\include" /external:I"%QT%\include\QtCore" /external:I"%QT%\include\QtGui" /external:I"%QT%\include\QtWidgets" tools\hvm_lab\hvm_ui_tests.cpp "%APP%\ArkDriverClient\ArkDriverHvm.cpp" "%APP%\UI\KvmControl.cpp" "%APP%\UI\KvmGuestVmPanel.cpp" "%APP%\KvmDock\KvmDock.cpp" "%APP%\KernelDock\KernelHvmTab.cpp" "%APP%\KernelDock\KernelHvmTab.Formatting.cpp" "%APP%\KernelDock\KernelHvmTab.Experimental.cpp" "%APP%\UI\FlowLayout.cpp" "%APP%\UI\ThemeStatusRole.cpp" "%APP%\Internationalization\LanguageManager.cpp" /Fo"%OUT%\\" /Fe"%OUT%\hvm_ui_tests.exe" /link /OPT:REF /LIBPATH:"%QT%\lib" Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib user32.lib advapi32.lib
if errorlevel 1 exit /b %errorlevel%
for %%D in (Qt6Core Qt6Gui Qt6Widgets) do copy /y "%QT%\bin\%%D.dll" "%OUT%\" >nul
set "PATH=%QT%\bin;%PATH%"
set "QT_PLUGIN_PATH=%QT%\plugins"
set "QT_QPA_PLATFORM=offscreen"
"%OUT%\hvm_ui_tests.exe" %*
exit /b %errorlevel%
