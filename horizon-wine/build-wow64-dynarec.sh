#!/bin/sh
# Opt-in dynarec runtime, using the same verified PE payload as the interpreter.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
sh "$root/horizon-wine/build-wow64-components.sh"
# USB drives as D: to H:, through libusbhsfs.
sh "$root/horizon-wine/tools/bootstrap-libusbhsfs.sh" >/dev/null
docker run --rm --platform linux/arm64 -v "$root:/work" -w /work \
    devkitpro/devkita64 sh -ec '
    cmake -S horizon-wine -B horizon-wine/build-switch-wow64-dynarec -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/work/horizon-wine/cmake/switch-devkitA64.cmake \
        -DWINE_NX_PE_BUILD_DIR=/work/horizon-wine/build-wine-wow64-pe \
        -DWINE_NX_BOX64_DYNAREC=ON -DWINE_NX_USB_STORAGE=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build horizon-wine/build-switch-wow64-dynarec --target wine-nx-runtime-nro -j 8
    '
python3 "$root/horizon-wine/tools/package-wow64-dynarec.py"
