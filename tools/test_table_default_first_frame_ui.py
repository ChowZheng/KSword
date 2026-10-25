#!/usr/bin/env python3
"""Exercise table discovery's first-paint boundary with real QtWidgets.

The production queue/filter branches are extracted unchanged. FloatingScrollbars,
ThemeBinding and the table viewport layout are production implementations. Only
the business action bar is a 32px strip adapter; this is not its business test.
Use --source-ref HEAD for the old first-paint failure. Windows uses HostX64 MSVC.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from xml.sax.saxutils import escape

from test_table_style_lifetime import block
from test_theme_first_frame_ui import qt_include

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "Ksword5.1/Ksword5.1"


def generate(source: str) -> str:
    current = "bool deferPendingConfigurations(QWidget* window)" in source
    replacements = {
        "LOOKUP": block(source, "    QTableView* tableForEventObject(QObject* watchedObject)"),
        "QUEUE": block(source, "    void scheduleTableConfiguration(QTableView* tableView)"),
        "DISCOVERY": block(source, "            if (eventType == QEvent::Show ||"),
        "GATE": block(source, "        bool deferPendingConfigurations(QWidget* window)") if current else "",
    }
    fixture = Path(__file__).with_name("table_default_first_frame_ui_tests.cpp").read_text(encoding="utf-8")
    fragments = {
        "PRODUCTION_TABLE_LOOKUP": replacements["LOOKUP"],
        "PRODUCTION_TABLE_QUEUE": replacements["QUEUE"],
        "PRODUCTION_TABLE_DISCOVERY": replacements["DISCOVERY"],
        "PRODUCTION_TABLE_GATE": replacements["GATE"],
        "PRODUCTION_READY_EVENT_TYPE": block(source, "    QEvent::Type initialTablePaintReadyEventType()")
            if current else "QEvent::Type initialTablePaintReadyEventType() { return QEvent::User; }",
        "PRODUCTION_READY_EVENT": block(source,
            "            if (eventObject->type() == initialTablePaintReadyEventType())") if current else "",
        "PRODUCTION_WINDOW_UPDATE": block(source,
            "            if (eventObject->type() == QEvent::UpdateRequest)") if current else "",
    }
    for marker, content in fragments.items():
        token = "// " + marker
        if fixture.count(token) != 1:
            raise SystemExit(f"Expected one production fragment marker: {marker}")
        fixture = fixture.replace(token, content)
    return fixture


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-dir", type=Path,
                        default=Path(os.environ.get("KSWORD_QT_DIR", "/usr")))
    parser.add_argument("--compiler")
    parser.add_argument("--msbuild", type=Path)
    parser.add_argument("--source-ref")
    args = parser.parse_args()
    path = "Ksword5.1/Ksword5.1/UI/TableInteractionSupport.cpp"
    production = (subprocess.check_output(["git", "show", f"{args.source_ref}:{path}"], cwd=ROOT)
                  .decode("utf-8-sig") if args.source_ref else (ROOT / path).read_text(encoding="utf-8-sig"))
    qt = args.qt_dir.resolve()
    include = qt_include(qt)
    includes = [APP, APP / "UI", include]
    includes.extend(include / module for module in ("QtCore", "QtGui", "QtWidgets"))
    with tempfile.TemporaryDirectory(prefix="ksword-table-default-first-") as temporary:
        work = Path(temporary)
        cpp = work / "TableDefaultFirstFrameTests.cpp"
        cpp.write_text(generate(production), encoding="utf-8")
        sources = [cpp, APP / "UI/FloatingScrollbars.cpp", APP / "UI/ThemeBinding.cpp"]
        if os.name != "nt":
            compiler = args.compiler or shutil.which("c++") or shutil.which("g++")
            if not compiler:
                parser.error("a native C++ compiler is required")
            executable = work / "TableDefaultFirstFrameTests"
            command = [compiler, "-std=c++20", "-fPIC", "-O1"]
            command.extend(f"-I{item}" for item in includes)
            command.extend(str(item) for item in sources)
            command.extend(["-l:libQt6Widgets.so.6", "-l:libQt6Gui.so.6",
                            "-l:libQt6Core.so.6", "-o", str(executable)])
            subprocess.run(command, check=True, timeout=120)
        else:
            if not args.msbuild or args.msbuild.resolve().parent.name.lower() != "amd64":
                parser.error("--msbuild must select 64-bit MSBuild")
            project = work / "TableDefaultFirstFrameTests.vcxproj"
            include_text = ";".join(escape(str(item)) for item in includes)
            source_text = "".join(f'<ClCompile Include="{escape(str(item))}" />' for item in sources)
            project.write_text(f'''<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<ItemGroup Label="ProjectConfigurations"><ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration></ItemGroup>
<PropertyGroup Label="Globals"><ProjectGuid>{{D0E99BB7-B2CA-42A0-A350-57C43C5F9C7B}}</ProjectGuid><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
<PropertyGroup Label="Configuration"><ConfigurationType>Application</ConfigurationType><PlatformToolset>v143</PlatformToolset><UseDebugLibraries>false</UseDebugLibraries></PropertyGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
<PropertyGroup><OutDir>$(ProjectDir)x64\\Release\\</OutDir><IntDir>$(ProjectDir)x64\\obj\\</IntDir></PropertyGroup>
<ItemDefinitionGroup><ClCompile><LanguageStandard>stdcpp20</LanguageStandard><RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary><PreprocessorDefinitions>NOMINMAX;UNICODE;_UNICODE;%(PreprocessorDefinitions)</PreprocessorDefinitions><AdditionalIncludeDirectories>{include_text};%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories><AdditionalOptions>/utf-8 /Zc:__cplusplus %(AdditionalOptions)</AdditionalOptions></ClCompile><Link><AdditionalLibraryDirectories>{escape(str(qt / 'lib'))};%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories><AdditionalDependencies>Qt6Widgets.lib;Qt6Gui.lib;Qt6Core.lib;%(AdditionalDependencies)</AdditionalDependencies><SubSystem>Console</SubSystem></Link></ItemDefinitionGroup>
<ItemGroup>{source_text}</ItemGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
</Project>''', encoding="utf-8")
            subprocess.run([str(args.msbuild.resolve()), str(project), "/t:Build",
                            "/p:Configuration=Release", "/p:Platform=x64",
                            "/p:PreferredToolArchitecture=x64", "/p:PROCESSOR_ARCHITECTURE=AMD64",
                            "/p:PROCESSOR_ARCHITEW6432=AMD64", "/m:1", "/v:minimal", "/nologo"],
                           check=True, timeout=180)
            executable = work / "x64/Release/TableDefaultFirstFrameTests.exe"
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        environment["PATH"] = str(qt / "bin") + os.pathsep + environment.get("PATH", "")
        environment["QT_PLUGIN_PATH"] = str(qt / "plugins" if os.name == "nt" else qt / "lib/qt6/plugins")
        for key, name in (("XDG_RUNTIME_DIR", "runtime"), ("XDG_CACHE_HOME", "cache")):
            directory = work / name
            directory.mkdir(mode=0o700)
            environment[key] = str(directory)
        subprocess.run([str(executable)], env=environment, check=True, timeout=60)


if __name__ == "__main__":
    main()
