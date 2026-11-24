#!/usr/bin/env bash
# Builds the Switch 2 (kaiHEN) core: cemu_core.nro (Vulkan/OpenGL), or the drako3d core with
# CEMU_SWITCH_DEKO3D_RUNTIME=ON. Needs ounce-devkit in $DEVKITPRO and the Switch 2 Mesa SDK (mesa-nx).
#
# Environment: SWITCH2_BUILD_DIR (build_switch2, or build_switch2_deko for the drako3d core), BUILD_JOBS,
#              CEMU_SWITCH_DEKO3D_RUNTIME (OFF), CEMU_RUNTIME_OUTPUT_DIR (<build dir>/bin), RELEASE_VERSION,
#              CEMU_SWITCH2_MESA_SDK_ZIP, CEMU_SWITCH2_MESA_SDK_ROOT.
set -euo pipefail

# MSYS2's cmake and ninja (devkitPro's CMake files refuse others, such as a CLANGARM64 shell's).
export PATH="/usr/bin:/bin:${PATH:-}"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
CEMU_SWITCH_DEKO3D_RUNTIME="${CEMU_SWITCH_DEKO3D_RUNTIME:-OFF}"
if [[ "${CEMU_SWITCH_DEKO3D_RUNTIME}" == ON ]]; then
	SWITCH2_BUILD_DIR="${SWITCH2_BUILD_DIR:-build_switch2_deko}"
else
	SWITCH2_BUILD_DIR="${SWITCH2_BUILD_DIR:-build_switch2}"
fi
case "${SWITCH2_BUILD_DIR}" in
	/*) BUILD_PATH="${SWITCH2_BUILD_DIR}" ;;
	*) BUILD_PATH="${ROOT}/${SWITCH2_BUILD_DIR}" ;;
esac
CEMU_RUNTIME_OUTPUT_DIR="${CEMU_RUNTIME_OUTPUT_DIR:-${BUILD_PATH}/bin}"
RELEASE_VERSION="${RELEASE_VERSION:-1.3.4}"
MESA_SDK_ROOT="${CEMU_SWITCH2_MESA_SDK_ROOT:-${ROOT}/dependencies/switch2_mesa_sdk}"
case "${BUILD_JOBS}" in
	''|*[!0-9]*|0) echo "BUILD_JOBS must be a positive integer" >&2; exit 2 ;;
esac
if [[ ! "$RELEASE_VERSION" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
	echo "RELEASE_VERSION must use major.minor.patch format" >&2
	exit 2
fi
VERSION_MAJOR="${BASH_REMATCH[1]}"
VERSION_MINOR="${BASH_REMATCH[2]}"
VERSION_PATCH="${BASH_REMATCH[3]}"

if [[ ! -f "${DEVKITPRO}/libounce/cmake/Switch2.cmake" ]]; then
	echo "No Switch 2 devkit (libounce) in ${DEVKITPRO}: install https://github.com/ouncebrew/ounce-devkit" >&2
	exit 1
fi

mkdir -p "${BUILD_PATH}/tmp"
# (portlibs/switch2/bin first: its pkg-config is the Switch 2's)
export PATH="${DEVKITPRO}/portlibs/switch2/bin:${DEVKITPRO}/tools/bin:${DEVKITPRO}/devkitA64/bin:${PATH}"
export TMPDIR="${BUILD_PATH}/tmp"
# devkitA64 uses Windows TEMP paths under MSYS2.
if command -v cygpath >/dev/null 2>&1; then
	SWITCH_TMP_WINDOWS="$(cygpath -w "${TMPDIR}")"
	export TEMP="${SWITCH_TMP_WINDOWS}" TMP="${SWITCH_TMP_WINDOWS}"
else
	export TEMP="${TMPDIR}" TMP="${TMPDIR}"
fi

cd "${ROOT}"
bash "${ROOT}/dist/switch/deps/prepare_submodules.sh"
bash "${ROOT}/dist/switch/deps/prepare_mesa_sdk_switch2.sh" \
	"${CEMU_SWITCH2_MESA_SDK_ZIP:-${ROOT}/../mesa-nx/dist/mesa-26.3.1-switch2-horizon-sdk.zip}" \
	"${MESA_SDK_ROOT}"

cmake -S . -B "${BUILD_PATH}" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/Toolchain-Switch2.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCEMU_SWITCH_DEKO3D_RUNTIME="${CEMU_SWITCH_DEKO3D_RUNTIME}" \
	-DCEMU_RUNTIME_OUTPUT_DIR="${CEMU_RUNTIME_OUTPUT_DIR}" \
	-DEMULATOR_VERSION_MAJOR="${VERSION_MAJOR}" \
	-DEMULATOR_VERSION_MINOR="${VERSION_MINOR}" \
	-DEMULATOR_VERSION_PATCH="${VERSION_PATCH}" \
	-DSWITCH_MESA_SDK_ROOT="${MESA_SDK_ROOT}" \
	-DSWITCH_LTO_JOBS="${BUILD_JOBS}"

cmake --build "${BUILD_PATH}" --parallel "${BUILD_JOBS}" --target CemuNro

echo ">> Output: ${CEMU_RUNTIME_OUTPUT_DIR}/cemu_core.nro"
