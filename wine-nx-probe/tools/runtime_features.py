#!/usr/bin/env python3
"""What a runtime built from this checkout can run, as the features the DLL
repository's manifest requires of it:

- unixlib:<name>: a native module the runtime's static unix-call table names;
- unixlib32:<name>: a WoW64 one its 32-bit table names;
- iface:<name>:<hash>: the headers runtime-interfaces.json lists for a module,
  which the module and the runtime were both built against.

horizon-dlls/tools/build-dlls.py writes what each file requires from this, and
the runtime's build writes the list it reports from it (--header), so the two
cannot come to disagree.

    runtime_features.py [--header OUT | --sources]
"""
from pathlib import Path
import argparse
import hashlib
import json
import re

root = Path(__file__).resolve().parents[2]
probe = root / 'wine-nx-probe'


def static_unix_libs():
    """The modules the runtime's static unix-call tables name: native ones by a
    wide string, WoW64 ones by a narrow one."""
    text = (root / 'dlls/ntdll/unix/virtual.c').read_text()
    native = text[text.index('wine_nx_static_unix_libs[] ='):]
    native = native[:native.index('};')]
    wow64 = text[text.index('wine_nx_static_wow64_unix_libs[] ='):]
    wow64 = wow64[:wow64.index('};')]
    return ({''.join(re.findall(r"'(.)'", entry)) for entry in re.findall(r'\{\s*\{([^}]*)\}', native)},
            set(re.findall(r'\{\s*"([^"]+)"', wow64)))


def interfaces():
    """iface:<name>:<hash> for each module runtime-interfaces.json lists."""
    table = json.loads((probe / 'runtime-interfaces.json').read_text())
    result = {}
    for name, files in table.items():
        if name.startswith('_'):
            continue
        digest = hashlib.sha256()
        for path in files:
            # As git holds it, so a Windows checkout's line ends hash the same.
            digest.update((root / path).read_bytes().replace(b'\r\n', b'\n'))
        result[name] = f'iface:{name}:{digest.hexdigest()[:12]}'
    return result


def interface_sources():
    table = json.loads((probe / 'runtime-interfaces.json').read_text())
    return sorted({path for name, files in table.items() if not name.startswith('_') for path in files})


def features():
    native, wow64 = static_unix_libs()
    return sorted({f'unixlib:{name}' for name in native} | {f'unixlib32:{name}' for name in wow64} |
                  set(interfaces().values()))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--header', type=Path, help='write them as a C header')
    parser.add_argument('--sources', action='store_true', help='list what they are read from, for a build')
    args = parser.parse_args()
    if args.sources:
        print(';'.join(str(root / path) for path in
                       ['dlls/ntdll/unix/virtual.c', 'wine-nx-probe/runtime-interfaces.json'] + interface_sources()))
    elif not args.header:
        print('\n'.join(features()))
    else:
        lines = ['/* The features this runtime reports to the DLL repository\'s manifest.',
                 ' * Written by wine-nx-probe/tools/runtime_features.py; do not edit. */',
                 '#ifndef HORIZON_DLL_FEATURES_H', '#define HORIZON_DLL_FEATURES_H', '',
                 'static const char *const horizon_dll_runtime_features[] =', '{']
        lines += [f'    "{feature}",' for feature in features()]
        lines += ['};', '', '#endif', '']
        text = '\n'.join(lines)
        if not args.header.exists() or args.header.read_text() != text:
            args.header.write_text(text)
