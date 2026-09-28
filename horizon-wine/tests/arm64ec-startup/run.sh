#!/bin/sh
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../../.." && pwd)
build=${BUILD_DIR:-"$root/horizon-wine/toolchains/build-arm64ec-startup"}
cc=${CC:-cc}

mkdir -p "$build"
python3 "$here/extract_startup.py" \
    "$root/dlls/ntdll/unix/signal_arm64.c" \
    "$root/horizon-wine/source/runtime.c" \
    "$build/extracted_startup.inc"
"$cc" -std=gnu11 -O2 -g -Wall -Wextra -Werror -I"$build" \
    "$here/startup_test.c" -o "$build/arm64ec-startup"
exec "$build/arm64ec-startup"
