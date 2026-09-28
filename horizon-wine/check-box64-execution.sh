#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
sh "$root/horizon-wine/tools/bootstrap-box64-core.sh"
docker run --rm --platform linux/arm64 -v "$root:/work" -w /work \
    devkitpro/devkita64 sh -ec '
    cmake -S horizon-wine/tests/box64-core -B horizon-wine/build-box64-core -G Ninja
    cmake --build horizon-wine/build-box64-core -j 8
    ctest --test-dir horizon-wine/build-box64-core --output-on-failure
    cmake -S horizon-wine/tests/box64-core -B horizon-wine/build-box64-core/switch -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/work/horizon-wine/cmake/switch-devkitA64.cmake
    cmake --build horizon-wine/build-box64-core/switch -j 8
    '
