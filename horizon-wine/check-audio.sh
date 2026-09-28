#!/bin/sh
# Host tests of the actual audout backend and registry wire adapter.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/wine-nx-audio.XXXXXX")"
trap 'rm -rf "$build"' EXIT HUP INT TERM
flags="-std=gnu11 -Wall -Wextra -Werror -D__WINESRC__ -DWINE_UNIX_LIB -D_WIN64 -fsanitize=address,undefined -fno-omit-frame-pointer"
"${CC:-clang}" $flags -I"$root/include" "$root/horizon-wine/tests/registry_server.c" -o "$build/registry_server"
"$build/registry_server"
"${CC:-clang}" $flags -I"$root/horizon-wine/tests/audio-shims" -I"$root/include" \
    -I"${WINE_NX_PE_BUILD_DIR:-$root/horizon-wine/build-wine-wow64-pe}/include" \
    "$root/horizon-wine/tests/audio_backend.c" -o "$build/audio"
"$build/audio"
