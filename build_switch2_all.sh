#!/bin/bash
# Builds the Switch 2 (kaiHEN) package: both cores and the launcher that embeds them (switch_launcher/cemu-nx2.nro).
# Needs ounce-devkit in $DEVKITPRO and the Switch 2 Mesa SDK zip (CEMU_SWITCH2_MESA_SDK_ZIP; default ../mesa-nx/dist/).
set -euo pipefail

BUILD_JOBS="${BUILD_JOBS:-18}"
RELEASE_VERSION="${RELEASE_VERSION:-1.3.2}"
case "${BUILD_JOBS}" in
	''|*[!0-9]*|0) echo "BUILD_JOBS must be a positive integer" >&2; exit 2 ;;
esac
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
# MSYS2's tools first (devkitPro's CMake files refuse a CLANGARM64 cmake).
export PATH="/usr/bin:/bin:${DEVKITPRO}/tools/bin:${DEVKITPRO}/devkitA64/bin:$PATH"
mkdir -p /tmp
export TMPDIR=/tmp TMP=/tmp TEMP=/tmp

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

echo "[1/4] Building the Switch 2 Vulkan/OpenGL core"
SWITCH2_BUILD_DIR=build_switch2 RELEASE_VERSION="$RELEASE_VERSION" BUILD_JOBS="$BUILD_JOBS" \
	bash "$ROOT/dist/switch/build-switch2.sh"

echo "[2/4] Building the Switch 2 drako3d core"
CEMU_SWITCH_DEKO3D_RUNTIME=ON SWITCH2_BUILD_DIR=build_switch2_deko \
	RELEASE_VERSION="$RELEASE_VERSION" BUILD_JOBS="$BUILD_JOBS" bash "$ROOT/dist/switch/build-switch2.sh"

echo "[3/4] Staging the Switch 2 cores"
ROMFS2="$ROOT/switch_launcher/romfs_nx2"
rm -rf "$ROMFS2"
mkdir -p "$ROMFS2/emu"
cp -f "$ROOT/switch_launcher/romfs/logo.png" "$ROMFS2/logo.png"
cp -f "$ROOT/build_switch2/bin/cemu_core.nro" "$ROMFS2/emu/cemu_core.nro"
sha256sum "$ROMFS2/emu/cemu_core.nro" | awk '{print $1}' > "$ROMFS2/emu/cemu_core.sha256"
# (the launcher keeps the "deko3d" renderer name for the drako3d core)
cp -f "$ROOT/build_switch2_deko/bin/cemu_core.nro" "$ROMFS2/emu/cemu_deko3d.nro"
sha256sum "$ROMFS2/emu/cemu_deko3d.nro" | awk '{print $1}' > "$ROMFS2/emu/cemu_deko3d.sha256"

echo "[4/4] Building the Switch 2 launcher"
cd "$ROOT/switch_launcher"
make -f Makefile.switch2 RELEASE_VERSION="$RELEASE_VERSION" APP_VERSION="$RELEASE_VERSION" clean
make -f Makefile.switch2 -j "$BUILD_JOBS" RELEASE_VERSION="$RELEASE_VERSION" APP_VERSION="$RELEASE_VERSION"

echo
echo "Copy switch_launcher/cemu-nx2.nro to sdmc:/switch/cemu/cemu-nx2.nro"
ls -la "$ROOT/switch_launcher/cemu-nx2.nro"
