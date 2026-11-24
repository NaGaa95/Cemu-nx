# Nintendo Switch build

The launcher and runtime use the lowercase directory `sdmc:/switch/cemu/`.
Install the final `cemu.nro` as `sdmc:/switch/cemu/cemu.nro`.

## Unified Mesa SDK

Extract the complete Mesa 26.2.0 Switch unified SDK into
`dependencies/switch_mesa_vulkan/`, then verify it with:

```sh
./dist/switch/deps/prepare_nvk.sh --check
```

The private driver payload is ignored by Git. The build localizes its archives
into one link object shared by native Vulkan and Zink, while native NVC0 and
Zink OpenGL are linked from the same SDK. The resulting core contains all three
rendering backends.

## LSFG-VK

LSFG support is built from `third_party/lsfg-vk`. Supply your own compatible
`Lossless.dll` at `sdmc:/switch/cemu/lsfg/Lossless.dll`. Enable LSFG preparation
in **Settings > Frame Generation**, then enable frame generation at runtime from
the in-game quick menu. It always starts disabled for each game.

The vendored LSFG-VK code is GPL-3.0-or-later; see
`third_party/lsfg-vk/LICENSE.md`.

## Build

Install the NTFS portlib along with your existing devkitPro dependencies:

```sh
dkp-pacman -S switch-ntfs-3g
```

Use `pacman` instead of `dkp-pacman` in devkitPro MSYS2. Both the core and
launcher require this package; CMake reports a missing dependency at configure
time. Then build both with:

```sh
BUILD_JOBS=18 ./build_switch_all.sh
```

The finished all-in-one launcher is `switch_launcher/cemu.nro`.

## USB HDD storage

FAT12/16/32, exFAT and NTFS drives are supported in both the SDL launcher and
the emulator. Select the drive in the existing USB file browser and add your
game folder as usual. The drive label includes its filesystem type. NTFS uses
the pinned libusbhsfs backend and the devkitPro NTFS-3G portlib, following
[Sphaira PR #361](https://github.com/NaGaa95/sphaira/pull/361). The existing
UASP transport and BOT fallback also apply to NTFS.

NTFS volumes mount with journal recovery and hidden-file browsing enabled.
Access-time updates are disabled to avoid metadata writes during game reads.
System files stay hidden and file read-only attributes are respected. Volumes
left hibernated by Windows are not forced to mount: fully shut down Windows
and safely eject the drive before connecting it. Use the launcher's **Safely
eject USB drive** action before unplugging it from the Switch.

NTFS support links the GPL-2.0-or-later NTFS-3G library; see the upstream
[libusbhsfs licensing notes](https://github.com/ITotalJustice/libusbhsfs#licensing).
EXT2/3/4 support remains disabled.

## Launcher updates

The SDL launcher checks the latest published release from
`NaGaa95/Cemu-nx`. Upload the all-in-one launcher as a `.nro` release asset;
`cemu.nro` is preferred when a release contains more than one NRO.

Set `RELEASE_VERSION` to the GitHub tag when making a release:

```sh
RELEASE_VERSION=1.3.4 BUILD_JOBS=18 ./build_switch_all.sh
```

The updater requires GitHub's SHA-256 asset digest and validates both the
digest and NRO structure before replacing the installed launcher.
