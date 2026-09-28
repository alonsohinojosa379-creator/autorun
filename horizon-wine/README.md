# horizon-wine

The Switch side of Autorun: the Horizon program that Wine runs inside. Wine
itself stays in the repository root, with its Horizon changes in place
(`dlls/ntdll/unix/horizon*`, `dlls/win32u/winnx_*`, `dlls/winebox64*`). This
folder builds the NRO around it: startup, the launcher, the display compositor,
the x86 and AMD64 CPU engines, audio, input and USB storage, and everything
needed to build, test and package it.

The Windows DLLs are not built or shipped here. They live in the DLL
repository, [autorun-horizon-dlls](https://github.com/autorunhq/autorun-horizon-dlls),
the `horizon-dlls/` submodule. The launcher downloads them to the card, and a
program starts only once the ones it needs are there and match this runtime's
interfaces (`runtime-interfaces.json`).

For how it all works, how to build it and what to run to test it, see
[documentation/technical.md](../documentation/technical.md). For players, see
the [README](../README.md).

## Runtimes

One source tree builds each runtime. The CMake options pick which:

| Runtime | Runs | Options |
|---|---|---|
| x86 (`nx-wow64-dynarec-*`) | 32-bit games through WoW64 and Box64 | `WINE_NX_BOX64_DYNAREC` |
| x86 with Mesa 26 | the same, with OpenGL and Vulkan from mesa-switch | `WINE_NX_MESA_SWITCH_DIR` |
| AMD64 (`nx-amd64-fex-*`) | 32- and 64-bit games through FEX (WoW64 and ARM64EC) | `WINE_NX_AMD64`, `WINE_NX_FEX` |

`tools/package-autorun.py` merges the x86 and AMD64 runtimes into the release
zip, `autorun-NNN.zip`. The build number is `WINE_NX_RUNTIME_BUILD` in
`source/runtime.c`.

## Quick build

From the repository root, with the requirements in
[technical.md](../documentation/technical.md#building):

```sh
sh horizon-wine/build-wow64-dynarec.sh          # x86 runtime
sh horizon-wine/build-mesa-switch.sh            # Mesa 26, for the Vulkan and DXVK runtimes
sh horizon-wine/build-amd64-components.sh       # AMD64 runtime (see technical.md for its environment)
python3 horizon-wine/tools/package-autorun.py   # autorun-NNN.zip
```

The build folders (`build-*`, `toolchains/`, `vendor/`) are not tracked.

## Layout

| Path | Content |
|---|---|
| `source/` | The runtime: `runtime.c` (startup), `launcher*.c` (the SDL launcher and its screens), `compositor*.c`, the Box64 and FEX engines, audio, XInput, USB storage, the forwarder installer, `horizon_dlls*.c` (the DLL manager) and `autorun_update.c` (self-update) |
| `tests/` | Host tests of the runtime and the Horizon server pieces, PE32 and PE32+ test programs, and `win32/`, small Windows programs the full package puts on the card |
| `tools/` | Packagers, game setup helpers, Box64/libusbhsfs/LSFG-VK bootstrap and `runtime_features.py`, which gives the features the DLL repository checks against |
| `cmake/` | The devkitA64 toolchain file, the Box64 core build and LSFG-VK |
| `fex/` | The Horizon patch for FEX and the ABI the runtime shares with it; FEX's DLLs are built in the DLL repository |
| `lsfg/`, `mesa/`, `mesosphere/`, `usbhsfs-uasp/` | Patches for LSFG-VK, Mesa and libdrm, the Atmosphère/HOC boot payloads and libusbhsfs |
| `hbl/` | nx-hbloader, for game forwarders |
| `assets/` | Launcher art and icons |
| `box64-shims/`, `switch-shims/`, `syntax/` | Headers that let Box64 and Wine's code build against libnx |
| `samples/7zr-x86/` | The x86 7-Zip console and archives the WoW64 test stage extracts |
| `build-*.sh`, `check-*.sh` | Build and test entry points |

## Launcher updates

Settings > System > Check for update reads the latest stable release of
`danfromtico/autorun`. A check at boot only shows a notification; installing
always asks first, and a development build newer than the release is not
downgraded.

The launcher checks the release zip's SHA-256, then extracts `switch/wine` over
the card. Configuration, games, saves, artwork and downloaded graphics versions
are kept; the runtime, fonts, NLS data and the NRO are replaced. An interrupted
install is rolled back before Wine loads, and backups stay in
`switch/wine/updates` until the new launcher opens. If the NRO no longer starts,
open `switch/wine/updates/previous.nro` from the Homebrew Menu.

The Windows DLLs update separately: Settings > System > Windows DLLs compares the card
with the DLL repository's manifest and downloads only what changed.

## History

This folder was `wine-nx-probe`, from when it was a probe that loaded Wine's
unix side on Horizon. Some CMake targets (`wine-nx-probe`, `wine-nx-pe-smoke`,
the ntdll smoke tests) and the `wine-nx-*` file names are from that time. The
per-build notes that used to be here are in the Git history.
