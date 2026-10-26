"""编译和调用驱动同源C规则的离线宿主；输入PE永不作为程序加载。"""
from __future__ import annotations
import hashlib, json, os, shutil, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'KswordARKDriver/src/features/keyboard'
PARTS = ('match','register','insert','remove','slot','loop','assignment','owner','flow','values','pe','decode')

def build_probe() -> Path:
    """完整源及编译选项hash复用宿主，不覆盖正在运行的旧探针。"""
    compiler = shutil.which(os.environ.get('CC','gcc'))
    if compiler is None:
        known = Path.home() / 'AppData/Local/Programs/CLion/bin/mingw/bin/gcc.exe'
        compiler = str(known) if known.is_file() else None
    if compiler is None:
        raise RuntimeError('An installed C compiler is required; no installation was attempted.')
    sources = [ROOT/'tools/hotkey_rule_probe.c', *(DRIVER/f'keyboard_rule_{name}.c' for name in PARTS), ROOT/'third_party/zydis/Zydis.c']
    headers = sorted(DRIVER.glob('keyboard_rule_*.h')) + [ROOT/'third_party/zydis/Zydis.h']
    options = ['-std=c11','-O2','-DZYDIS_STATIC_BUILD','-DZYCORE_STATIC_BUILD','-DZYAN_NO_LIBC']
    digest = hashlib.sha256((compiler+repr(options)).encode())
    for source in [*sources,*headers]:
        digest.update(str(source.relative_to(ROOT)).encode()); digest.update(source.read_bytes())
    directory = ROOT/'.codex-build-logs/hotkey-rule-host'; directory.mkdir(parents=True,exist_ok=True)
    executable = directory/('rule-probe-'+digest.hexdigest()[:20]+('.exe' if os.name=='nt' else ''))
    if executable.is_file() and executable.stat().st_size: return executable
    command = [compiler,*options,'-I',str(ROOT/'third_party/zydis'),*(str(p) for p in sources),'-o',str(executable)]
    completed = subprocess.run(command,cwd=ROOT,capture_output=True,text=True,timeout=240)
    if completed.returncode: raise RuntimeError(completed.stderr[-12000:])
    return executable

def match_pair(full: Path, base: Path, binary: Path|None=None) -> dict:
    """调用生产入口；版本/GUID/SHA不作支持条件。"""
    completed = subprocess.run([str(binary or build_probe()),str(full.resolve()),str(base.resolve())],cwd=ROOT,capture_output=True,text=True,timeout=180)
    if completed.returncode: return {'Matched':False,'ErrorName':'host_input_rejected','detail':completed.stderr.strip()}
    return json.loads(completed.stdout)
