#!/usr/bin/env python3
"""Check real ThemeBinding/ToolbarMetrics first-paint geometry with QtWidgets.

Compiles the production ThemeBinding translation unit. MainWindow surface and
standalone-input role adapters are inert because the fixture has no such host.
Use --production-source to run the same behavioral checks against an old source
snapshot as a negative control. Windows builds use the project's HostX64 MSVC.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "Ksword5.1/Ksword5.1"


def qt_include(qt: Path) -> Path:
    choices = [qt / "include", qt / "include/qt6"]
    choices.extend(sorted((qt / "include").glob("*/qt6")))
    for candidate in choices:
        if (candidate / "QtWidgets/QApplication").is_file():
            return candidate
    raise SystemExit(f"Qt 6 development headers unavailable under {qt}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-dir", "--qt-root", dest="qt", type=Path,
                        default=Path(os.environ.get("KSWORD_QT_DIR", "/usr")))
    parser.add_argument("--compiler", "--cxx", dest="compiler")
    parser.add_argument("--msbuild", type=Path)
    parser.add_argument("--production-source", type=Path,
                        default=APP / "UI/ThemeBinding.cpp")
    args = parser.parse_args()
    qt = args.qt.resolve()
    include = qt_include(qt)
    sources = [Path(__file__).with_name("theme_first_frame_ui_tests.cpp"),
               args.production_source.resolve()]
    includes = [APP, APP / "UI", include]
    includes.extend(include / module for module in ("QtCore", "QtGui", "QtWidgets"))
    with tempfile.TemporaryDirectory(prefix="ksword-theme-first-frame-") as temporary:
        work = Path(temporary)
        settings_source = (APP / "UI/SettingsPresentation.cpp").read_text(encoding="utf-8-sig")
        marker = '#include "../Framework.h"'
        if settings_source.count(marker) != 1:
            raise SystemExit("Expected one Windows umbrella include in SettingsPresentation")
        settings = work / "SettingsPresentation.test.cpp"
        settings.write_text(settings_source.replace(marker, '#include <QWidget>'), encoding="utf-8")
        sources.append(settings)
        if os.name != "nt":
            compiler = args.compiler or shutil.which("c++") or shutil.which("g++")
            if not compiler:
                parser.error("a native C++ compiler is required")
            executable = work / "ThemeFirstFrameTests"
            command = [compiler, "-std=c++20", "-fPIC", "-O1"]
            command.extend(f"-I{item}" for item in includes)
            command.extend(str(item) for item in sources)
            command.extend(["-l:libQt6Widgets.so.6", "-l:libQt6Gui.so.6",
                            "-l:libQt6Core.so.6", "-o", str(executable)])
            subprocess.run(command, check=True, timeout=120)
        else:
            if not args.msbuild or args.msbuild.resolve().parent.name.lower() != "amd64":
                parser.error("--msbuild must select 64-bit MSBuild")
            project = work / "ThemeFirstFrameTests.vcxproj"
            include_text = ";".join(escape(str(item)) for item in includes)
            source_text = "".join(f'<ClCompile Include="{escape(str(item))}" />' for item in sources)
            project.write_text(f'''<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<ItemGroup Label="ProjectConfigurations"><ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration></ItemGroup>
<PropertyGroup Label="Globals"><ProjectGuid>{{06183D0F-8667-4AC3-92A6-1B5F9C440607}}</ProjectGuid><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
<PropertyGroup Label="Configuration"><ConfigurationType>Application</ConfigurationType><PlatformToolset>v143</PlatformToolset><UseDebugLibraries>false</UseDebugLibraries></PropertyGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
<PropertyGroup><OutDir>$(ProjectDir)x64\\Release\\</OutDir><IntDir>$(ProjectDir)x64\\obj\\</IntDir></PropertyGroup>
<ItemDefinitionGroup><ClCompile><LanguageStandard>stdcpp20</LanguageStandard><ConformanceMode>true</ConformanceMode><RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary><PreprocessorDefinitions>NOMINMAX;UNICODE;_UNICODE;%(PreprocessorDefinitions)</PreprocessorDefinitions><AdditionalIncludeDirectories>{include_text};%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories><AdditionalOptions>/utf-8 /Zc:__cplusplus %(AdditionalOptions)</AdditionalOptions></ClCompile><Link><AdditionalLibraryDirectories>{escape(str(qt / 'lib'))};%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories><AdditionalDependencies>Qt6Widgets.lib;Qt6Gui.lib;Qt6Core.lib;%(AdditionalDependencies)</AdditionalDependencies><SubSystem>Console</SubSystem></Link></ItemDefinitionGroup>
<ItemGroup>{source_text}</ItemGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
</Project>''', encoding="utf-8")
            properties = ["/p:Configuration=Release", "/p:Platform=x64",
                          "/p:PreferredToolArchitecture=x64", "/p:PROCESSOR_ARCHITECTURE=AMD64",
                          "/p:PROCESSOR_ARCHITEW6432=AMD64"]
            inspect = subprocess.run([str(args.msbuild), str(project), *properties,
                "-getProperty:PreferredToolArchitecture,VC_ExecutablePath_x64", "/nologo"],
                check=True, text=True, capture_output=True, timeout=60)
            selected = json.loads(inspect.stdout)["Properties"]
            tools = selected["VC_ExecutablePath_x64"].replace("\\", "/").lower()
            if selected["PreferredToolArchitecture"] != "x64" or "hostx64/x64" not in tools:
                raise SystemExit("HostX64 MSVC is required")
            subprocess.run([str(args.msbuild), str(project), *properties,
                            "/t:Build", "/m:1", "/v:minimal", "/nologo"],
                           check=True, timeout=180)
            executable = work / "x64/Release/ThemeFirstFrameTests.exe"
        runtime = work / "runtime"
        cache = work / "cache"
        runtime.mkdir(mode=0o700)
        cache.mkdir()
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen",
                           XDG_RUNTIME_DIR=str(runtime), XDG_CACHE_HOME=str(cache))
        if os.name == "nt":
            environment["PATH"] = str(qt / "bin") + os.pathsep + environment.get("PATH", "")
            environment["QT_PLUGIN_PATH"] = str(qt / "plugins")
        subprocess.run([str(executable)], env=environment, check=True, timeout=30)


if __name__ == "__main__":
    main()
