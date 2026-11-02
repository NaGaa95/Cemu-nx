#!/usr/bin/env bash
set -euo pipefail

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
SWITCH_BUILD_DIR="${SWITCH_BUILD_DIR:-build_switch}"
CEMU_SWITCH_DEKO3D_RUNTIME="${CEMU_SWITCH_DEKO3D_RUNTIME:-OFF}"
CEMU_RUNTIME_OUTPUT_DIR="${CEMU_RUNTIME_OUTPUT_DIR:-${ROOT}/bin}"
CEMU_SWITCH_PGO="${CEMU_SWITCH_PGO:-USE}"
RELEASE_VERSION="${RELEASE_VERSION:-1.3.1}"
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
cd "${ROOT}"

bash "${ROOT}/dist/switch/deps/prepare_submodules.sh"

if [[ "$CEMU_SWITCH_DEKO3D_RUNTIME" != ON ]]; then
	# Use the newest complete Mesa SDK unless explicitly overridden.
	if [[ -z "${SWITCH_MESA_SDK_ROOT:-}" ]]; then
		newest_mtime=0
		for candidate in "${ROOT}"/dependencies/switch_mesa_sdk_*/opt/devkitpro/portlibs/switch; do
			[[ -d "${candidate}" ]] || continue
			[[ -f "${candidate}/share/mesa-switch/revision" ]] || continue
			[[ -f "${candidate}/lib/libnvk.a" ]] || continue
			candidate_mtime="$(stat -c %Y "${candidate}" 2>/dev/null || echo 0)"
			if (( candidate_mtime >= newest_mtime )); then
				newest_mtime="${candidate_mtime}"
				SWITCH_MESA_SDK_ROOT="${candidate}"
			fi
		done
	fi
	if [[ -z "${SWITCH_MESA_SDK_ROOT:-}" || ! -f "${SWITCH_MESA_SDK_ROOT}/lib/libnvk.a" ]]; then
		echo "No Mesa SDK found. Extract one into dependencies/switch_mesa_sdk_<date>/," >&2
		echo "or point SWITCH_MESA_SDK_ROOT at a portlibs/switch directory." >&2
		exit 2
	fi
	export SWITCH_MESA_SDK_ROOT
	MESA_REVISION="$(tr -d '\r\n' < "${SWITCH_MESA_SDK_ROOT}/share/mesa-switch/revision" 2>/dev/null || true)"
	echo ">> Mesa SDK: ${SWITCH_MESA_SDK_ROOT}${MESA_REVISION:+ (${MESA_REVISION})}"

	# Rebuild when the Mesa SDK changes to avoid mixing driver objects.
	CACHED_MESA_SDK_ROOT=""
	if [[ -f "${SWITCH_BUILD_DIR}/CMakeCache.txt" ]]; then
		CACHED_MESA_SDK_ROOT="$(sed -n 's/^SWITCH_MESA_SDK_ROOT:[^=]*=//p' \
			"${SWITCH_BUILD_DIR}/CMakeCache.txt" | head -1)"
	fi
	if [[ -n "${CACHED_MESA_SDK_ROOT}" && "${CACHED_MESA_SDK_ROOT}" != "${SWITCH_MESA_SDK_ROOT}" ]]; then
		echo ">> Mesa SDK changed from ${CACHED_MESA_SDK_ROOT}; rebuilding from scratch"
		rm -rf "${SWITCH_BUILD_DIR}"
	fi

	echo ">> Localizing NVK driver symbols ..."
	bash "${ROOT}/dist/switch/deps/prepare_nvk.sh"
fi

cmake -S . -B "${SWITCH_BUILD_DIR}" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/Toolchain-Switch.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCEMU_SWITCH_DEKO3D_RUNTIME="${CEMU_SWITCH_DEKO3D_RUNTIME}" \
	-DCEMU_RUNTIME_OUTPUT_DIR="${CEMU_RUNTIME_OUTPUT_DIR}" \
	-DCEMU_SWITCH_PGO="${CEMU_SWITCH_PGO}" \
	-DCEMU_SWITCH_PGO_DIR="${CEMU_SWITCH_PGO_DIR:-}" \
	-DEMULATOR_VERSION_MAJOR="${VERSION_MAJOR}" \
	-DEMULATOR_VERSION_MINOR="${VERSION_MINOR}" \
	-DEMULATOR_VERSION_PATCH="${VERSION_PATCH}" \
	-DENABLE_OPENGL=ON \
	-DENABLE_VULKAN=ON \
	-DSWITCH_MESA_SDK_ROOT="${SWITCH_MESA_SDK_ROOT:-}" \
	-DSWITCH_LTO_JOBS="${BUILD_JOBS}"

cmake --build "${SWITCH_BUILD_DIR}" --parallel "${BUILD_JOBS}" --target CemuNro

echo ">> Output: ${CEMU_RUNTIME_OUTPUT_DIR}/cemu_core.nro"
