#!/bin/bash
set -euo pipefail

BUILD_JOBS="${BUILD_JOBS:-18}"
RELEASE_VERSION="${RELEASE_VERSION:-1.2.0}"
case "${BUILD_JOBS}" in
	''|*[!0-9]*|0) echo "BUILD_JOBS must be a positive integer" >&2; exit 2 ;;
esac
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="/usr/bin:${DEVKITPRO}/tools/bin:${DEVKITPRO}/devkitA64/bin:$PATH"
mkdir -p /tmp
export TMPDIR=/tmp TMP=/tmp TEMP=/tmp

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

echo "[1/5] Building the Vulkan/OpenGL emulator"
RELEASE_VERSION="$RELEASE_VERSION" BUILD_JOBS="$BUILD_JOBS" bash "$ROOT/dist/switch/build.sh"

echo "[2/5] Building the Deko3D emulator"
CEMU_SWITCH_DEKO3D_RUNTIME=ON SWITCH_BUILD_DIR=build_switch_deko \
	CEMU_RUNTIME_OUTPUT_DIR="$ROOT/build_switch_deko/bin" \
	RELEASE_VERSION="$RELEASE_VERSION" BUILD_JOBS="$BUILD_JOBS" bash "$ROOT/dist/switch/build.sh"

echo "[3/5] Staging the emulators"
mkdir -p "$ROOT/switch_launcher/romfs/emu"
rm -f "$ROOT/switch_launcher/romfs/emu/cemu_vk.nro" \
	"$ROOT/switch_launcher/romfs/emu/cemu_vk.sha256" \
	"$ROOT/switch_launcher/romfs/emu/cemu_gl.nro" \
	"$ROOT/switch_launcher/romfs/emu/cemu_gl.sha256" \
	"$ROOT/switch_launcher/romfs/emu/cemu_zink.nro" \
	"$ROOT/switch_launcher/romfs/emu/cemu_zink.sha256"
cp -f "$ROOT/bin/cemu_core.nro" "$ROOT/switch_launcher/romfs/emu/cemu_core.nro"
sha256sum "$ROOT/switch_launcher/romfs/emu/cemu_core.nro" | awk '{print $1}' > "$ROOT/switch_launcher/romfs/emu/cemu_core.sha256"
cp -f "$ROOT/build_switch_deko/bin/cemu_core.nro" "$ROOT/switch_launcher/romfs/emu/cemu_deko3d.nro"
sha256sum "$ROOT/switch_launcher/romfs/emu/cemu_deko3d.nro" | awk '{print $1}' > "$ROOT/switch_launcher/romfs/emu/cemu_deko3d.sha256"

echo "[4/5] Building the HOME Menu forwarder"
make -C "$ROOT/switch_launcher/fwd" VERSION="$RELEASE_VERSION" clean
make -j "$BUILD_JOBS" -C "$ROOT/switch_launcher/fwd" VERSION="$RELEASE_VERSION"

echo "[5/5] Building the launcher"
MESA20_STAGE="$ROOT/build_switch/launcher_mesa20"
MESA20_ROOT="$MESA20_STAGE/opt/devkitpro/portlibs/switch"
MESA20_PACKAGE="${LAUNCHER_MESA20_PACKAGE:-}"
if [[ -z "$MESA20_PACKAGE" ]]; then
	shopt -s nullglob
	mesa20_packages=(/var/cache/pacman/pkg/switch-mesa-20.1.0-*.pkg.tar.zst)
	shopt -u nullglob
	if ((${#mesa20_packages[@]})); then
		MESA20_PACKAGE="${mesa20_packages[${#mesa20_packages[@]}-1]}"
	fi
fi
if [[ -n "$MESA20_PACKAGE" && -f "$MESA20_PACKAGE" ]]; then
	mkdir -p "$MESA20_STAGE"
	tar -xf "$MESA20_PACKAGE" -C "$MESA20_STAGE"
elif grep -q '^Version: 20\.1\.0' "$DEVKITPRO/portlibs/switch/lib/pkgconfig/glesv2.pc" 2>/dev/null; then
	MESA20_ROOT="$DEVKITPRO/portlibs/switch"
else
	echo "Stock switch-mesa 20.1.0 is required for the SDL launcher." >&2
	echo "Cache it with: dkp-pacman -Sw switch-mesa" >&2
	exit 1
fi
cd "$ROOT/switch_launcher"
make LAUNCHER_MESA20_ROOT="$MESA20_ROOT" RELEASE_VERSION="$RELEASE_VERSION" APP_VERSION="$RELEASE_VERSION" clean
make -j "$BUILD_JOBS" LAUNCHER_MESA20_ROOT="$MESA20_ROOT" RELEASE_VERSION="$RELEASE_VERSION" APP_VERSION="$RELEASE_VERSION"

echo
echo "Copy switch_launcher/cemu.nro to sdmc:/switch/cemu/cemu.nro"
ls -la "$ROOT/switch_launcher/cemu.nro"
