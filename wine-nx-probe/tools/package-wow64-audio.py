#!/usr/bin/env python3
"""Stage the native audio checkpoint over the existing GUI package."""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import functools
import os
import re
import shutil
import subprocess
import sys

probe = Path(__file__).resolve().parents[1]
pe = probe / 'build-wine-wow64-pe'
build = probe / 'build-switch-wow64-dynarec'
base = Path(os.environ.get('WINE_NX_AUDIO_BASE', build / 'switch/wine'))
stage_root = build / 'audio-sd-card'
stage = stage_root / 'switch/wine'
tools = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'
env = dict(os.environ, PATH=f'{tools}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])
if not (base / 'drive_c/notepad-test.txt').is_file():
    raise SystemExit('Run package-wow64-notepad.py first.')
# The test program, with no CRT dependency. The driver it plays through,
# winenxaudio.drv, is the DLL repository's.
common = [str(tools / 'i686-w64-mingw32-clang'), '-Os', '-Wall', '-Wextra', '-Werror',
          '-fno-builtin', '-nostdlib']
exe = pe / 'pe32-audio.exe'
subprocess.run(common + ['-Wl,--entry,_start@0', '-Wl,--image-base,0x10000000', '-Wl,--dynamicbase',
    '-o', str(exe), str(probe / 'tests/pe32_audio.c'), '-lwinmm', '-lkernel32', '-lntdll'], check=True, env=env)
shutil.copytree(base, stage, dirs_exist_ok=True, ignore=shutil.ignore_patterns('*.log', '.DS_Store'))
shutil.copy2(build / 'wine-nx-runtime.nro', stage / 'wine-nx-runtime.nro')
shutil.copy2(exe, stage / 'drive_c/pe32-audio.exe')
(stage / 'drive_c/pe32-audio.args.txt').write_text('\n')
(stage / 'target.txt').write_text('sdmc:/switch/wine/drive_c/pe32-audio.exe\n')
(stage / 'run-entry.txt').write_text('1\n')
(stage / 'AUDIO-README.txt').write_text('''Wine-NX build 34: first native audout playback checkpoint.
Copy the entire switch/wine folder to the SD card, merging folders. The new
winenxaudio.drv, mmdevapi.dll, winmm.dll and their dependencies come from the
DLL repository (horizon-dlls).
Choose C:\\pe32-audio.exe in the launcher. Expected: a quiet half-second tone
on the left followed by a half-second tone on the right, then [AUDIO TEST]
PASS with process exit 42. API completion alone does not prove audible output.

Scope: one render client with stereo 48 kHz 16-bit output, event callbacks,
volume, stop and reset. Mono or stereo PCM of 8 to 32 bits and float input are
converted, and other rates such as 44.1 kHz are resampled to 48 kHz. No
microphone, MIDI synthesis or multi-client mixing yet. The audio driver and
MMDeviceEnumerator are registered at startup, and registry changes are saved
to system.reg and user.reg in sdmc:/switch/wine/registry.

OpenTTD plays sound effects with -s win32 -m null, which the OpenTTD package
selects, with the OpenSFX base set.
''')
subprocess.run([sys.executable, str(probe / 'tools/verify-wow64-package.py'), str(stage)], check=True)
# The full package stages every checkpoint over the one before it and has only one
# archive to give; asked for a stage alone, this leaves its own unwritten.
stage_only = os.environ.get('WINE_NX_STAGE_ONLY') == '1'
if stage_only:
    print(stage_root)
else:
    archive = build / 'wine-nx-audio-dynarec-34.zip'
    with ZipFile(archive, 'w', ZIP_DEFLATED) as z:
        for f in sorted(stage.rglob('*')):
            if f.is_file() and f.name != '.DS_Store' and f.suffix != '.log':
                z.write(f, f.relative_to(stage_root))
    with ZipFile(archive) as z:
        assert z.testzip() is None
    print(archive)
