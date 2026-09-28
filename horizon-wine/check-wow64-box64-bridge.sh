#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/wine-nx-wow64.XXXXXX")"
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CC:-clang}" -std=gnu11 -fms-extensions -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -D__WINESRC__ -D_WIN64 \
    -I"$root/include" -I"$root/dlls/ntdll/unix" -I"$root/horizon-wine/source" \
    "$root/horizon-wine/source/wow64_box64_bridge.c" \
    "$root/horizon-wine/tests/wow64_box64_bridge.c" -o "$build/check"
"$build/check"
"${CC:-clang}" -std=gnu11 -fms-extensions -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -D__WINESRC__ -D_WIN64 -I"$root/include" \
    "$root/horizon-wine/tests/winebox64_cpuid.c" -o "$build/cpuid"
"$build/cpuid"
