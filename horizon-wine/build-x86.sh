#!/bin/sh
# The x86 runtime Autorun ships: 32-bit programs through WoW64 and the Box64
# dynarec, linked with mesa-switch (OpenGL through nvc0, Vulkan through NVK),
# into build-switch-wow64-mesa-switch. Build Mesa (build-mesa-switch.sh) and the
# AMD64 runtime (build-amd64-components.sh, which configures the Wine tree whose
# headers both runtimes compile against) first.
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
pe="$root/horizon-wine/build-wine-amd64-pe"
mesa="${WINE_NX_MESA_SWITCH_DIR:-/work/horizon-wine/build-mesa-switch/install/opt/devkitpro/portlibs/switch/lib}"
export PATH="$root/horizon-wine/toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin:/opt/homebrew/opt/bison/bin:$PATH"
if [ ! -f "$pe/Makefile" ]; then
    echo "No Wine tree in build-wine-amd64-pe; build-amd64-components.sh configures it." >&2
    exit 1
fi
[ -d "$root/${mesa#/work/}" ] || { echo "No Mesa in ${mesa#/work/}; run build-mesa-switch.sh." >&2; exit 1; }
# The headers the runtime compiles against. The Windows modules it runs are the
# DLL repository's (horizon-dlls/tools/build-dlls.py); none are built here.
make -C "$pe" -j8 include/all
sh "$root/horizon-wine/tools/bootstrap-box64-core.sh"
# USB drives as D: to H:, through libusbhsfs.
sh "$root/horizon-wine/tools/bootstrap-libusbhsfs.sh" >/dev/null
sh "$root/horizon-wine/tools/bootstrap-lsfg-vk.sh"
docker run --rm --platform linux/arm64 -v "$root:/work" -w /work -e NX_MESA="$mesa" \
    "${WINE_NX_DEVKIT_IMAGE:-devkitpro-lsfg}" sh -ec '
    cmake -S horizon-wine -B horizon-wine/build-switch-wow64-mesa-switch -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/work/horizon-wine/cmake/switch-devkitA64.cmake \
        -DWINE_NX_PE_BUILD_DIR=/work/horizon-wine/build-wine-amd64-pe \
        -DWINE_NX_BOX64_DYNAREC=ON -DWINE_NX_USB_STORAGE=ON -DCMAKE_BUILD_TYPE=Release \
        -DWINE_NX_MESA_SWITCH_DIR="$NX_MESA"
    cmake --build horizon-wine/build-switch-wow64-mesa-switch --target wine-nx-runtime-nro -j 8
    '
