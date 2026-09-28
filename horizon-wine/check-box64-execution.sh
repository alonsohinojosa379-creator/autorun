#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
sh "$root/horizon-wine/tools/bootstrap-box64-core.sh"
docker run --rm --platform linux/arm64 -v "$root:/work" -w /work \
    devkitpro/devkita64 sh -ec '
    # Built in the container, and gone with it.
    cmake -S horizon-wine/tests/box64-core -B /tmp/box64-core -G Ninja
    cmake --build /tmp/box64-core -j 8
    ctest --test-dir /tmp/box64-core --output-on-failure
    cmake -S horizon-wine/tests/box64-core -B /tmp/box64-core-switch -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/work/horizon-wine/cmake/switch-devkitA64.cmake
    cmake --build /tmp/box64-core-switch -j 8
    '
