"""Run public-unregister result/argument tests without loading a driver."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--cc', default='gcc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source_dir = root / 'KswordARKDriver/src/features/callback'
with tempfile.TemporaryDirectory(prefix='ksword-callback-remove-') as directory:
    temporary = Path(directory)
    # Compile production statements verbatim; only replace the WDK include with mock declarations.
    backend = (source_dir / 'callback_remove_extended.c').read_text(encoding='utf-8')
    backend = backend.replace('#include "callback_internal.h"', '/* Mock declarations supplied by harness. */')
    backend = backend.replace('#include "callback_extended_internal.h"', '/* Mock export resolver. */')
    backend = backend.replace('#include "callback_external_minifilter.h"', '/* Mock public filter unload backend. */')
    registry = (source_dir / 'callback_registry_identity.c').read_text(encoding='utf-8').replace('#include "callback_internal.h"', '/* Mock runtime and reader. */')
    identity = (source_dir / 'callback_special_identity.h').read_text(encoding='utf-8').replace('#pragma once', '')
    snapshot = (source_dir / 'callback_snapshot.c').read_text(encoding='utf-8')
    start = snapshot.index('static BOOLEAN\nKswordArkCallbackEnumRemoveRequestMatchesEntry(')
    end = snapshot.index('\nVOID\nKswordArkCallbackEnumSnapshotBegin(', start)
    (temporary / 'production.c').write_text(backend + '\n' + identity + '\n' + registry + '\n' + snapshot[start:end], encoding='utf-8')
    shutil.copyfile(Path(__file__).with_name('harness.c'), temporary / 'harness.c')
    executable = temporary / 'callback_remove_tests.exe'
    if Path(args.cc).stem.lower() == 'cl':
        command = [args.cc, '/nologo', '/std:c11', '/W4', '/WX', '/utf-8',
                   '/I' + str(root / 'shared/driver'), str(temporary / 'harness.c'),
                   '/Fe:' + str(executable), '/Fo:' + str(temporary / 'harness.obj')]
    else:
        command = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                   '-I', str(root / 'shared/driver'), str(temporary / 'harness.c'),
                   '-o', str(executable)]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)
