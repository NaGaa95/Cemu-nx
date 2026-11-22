#!/usr/bin/env bash
# Stage the Nintendo Switch 2 Mesa SDK (mesa-*-switch2-horizon-sdk.zip, with uam2rt) for the Switch 2 build
# (dist/switch/build-switch2.sh). The Switch build's SDK (dependencies/switch_mesa_sdk_*) is not touched.
#
# The Switch 2 SDK's libEGL.a and libvulkan.a are GNU ld linker scripts naming libmesa_egl.a, libmesa_gl.a,
# libmesa_glapi.a and libmesa_vulkan.a (NVK). Cemu's Vulkan dispatch table defines public vk* function-pointer
# variables, so NVK's public vk* functions must not stay global in the link (dist/switch/localize_nvk.sh does the
# same for the Switch SDK). The GL and Vulkan archives also carry the same ~3600 Mesa utility symbols, so NVK cannot be
# merged alone without duplicating them: everything is merged into one relocatable object in the linker scripts'
# order, which picks the same archive members as a normal link, and only then are the public vk* entry points (and
# Mesa's writev and posix_memalign shims, which Cemu and libounce provide) localized. lib/libEGL.a and lib/libvulkan.a
# are rewritten to name that object's archive.
#
# Usage: prepare_mesa_sdk_switch2.sh [mesa-switch2-sdk.zip] [output-directory]
#        prepare_mesa_sdk_switch2.sh --check [output-directory]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CHECK_ONLY=0
if [[ "${1:-}" == "--check" ]]; then
	CHECK_ONLY=1
	shift
	OUT="${1:-${CEMU_SWITCH2_MESA_SDK_ROOT:-${ROOT}/dependencies/switch2_mesa_sdk}}"
	for required in libvulkan_local.o libvulkan_local.a lib/libmesa_local.a lib/libEGL.a lib/libvulkan.a \
		include/EGL/egl.h lib/cmake/OpenGL/OpenGLConfig.cmake lib/cmake/uam2rt/uam2rtConfig.cmake \
		lib/libuam2rt.a include/uam2rt.h; do
		if [[ ! -f "${OUT}/${required}" ]]; then
			echo "error: Switch 2 Mesa SDK is incomplete: ${OUT}/${required} is missing" >&2
			echo "Run dist/switch/deps/prepare_mesa_sdk_switch2.sh first." >&2
			exit 1
		fi
	done
	echo "Ready: Switch 2 Mesa SDK at ${OUT#${ROOT}/}"
	exit 0
fi

DEFAULT_DIST="${ROOT}/../mesa-nx/dist"
ZIP="${1:-${CEMU_SWITCH2_MESA_SDK_ZIP:-${DEFAULT_DIST}/mesa-26.3.1-switch2-horizon-sdk.zip}}"
OUT="${2:-${CEMU_SWITCH2_MESA_SDK_ROOT:-${ROOT}/dependencies/switch2_mesa_sdk}}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BIN="${DEVKITPRO}/devkitA64/bin"
LD="${BIN}/aarch64-none-elf-ld"
OBJCOPY="${BIN}/aarch64-none-elf-objcopy"
NM="${BIN}/aarch64-none-elf-nm"
AR="${BIN}/aarch64-none-elf-ar"

if command -v bsdtar >/dev/null 2>&1; then
	BSDTAR="$(command -v bsdtar)"
elif [[ -x /c/Windows/System32/tar.exe ]]; then
	BSDTAR=/c/Windows/System32/tar.exe
else
	echo "A bsdtar-compatible archive tool is required to read the SDK zips" >&2
	exit 1
fi

if [[ ! -f "${ZIP}" ]]; then
	echo "Usage: $0 [mesa-switch2-horizon-sdk.zip] [output-directory]" >&2
	echo "Missing: ${ZIP}" >&2
	exit 1
fi

# The prepared prefix is reused while the zip is unchanged (a new libvulkan_local.o would relink everything).
STAMP_VALUE="$(sha256sum "${ZIP}" "${BASH_SOURCE[0]}" | awk '{ print $1 }' | tr '\n' ' ')"
STAMP="${OUT}/share/cemu-switch2-sdk.stamp"
if [[ -f "${STAMP}" && "$(cat "${STAMP}")" == "${STAMP_VALUE}" && -f "${OUT}/libvulkan_local.o" &&
	-f "${OUT}/lib/libuam2rt.a" ]]; then
	echo "Switch 2 Mesa SDK is up to date: ${OUT}"
	exit 0
fi

# The prefix inside an SDK zip: the directory holding lib/cmake/<package>/<file>.
zip_prefix() {
	"${BSDTAR}" -tf "$1" | awk -v marker="$2" '
		!found && index($0, marker) {
			sub(marker "$", "");
			print;
			found = 1;
		}'
}

SDK_ROOT="$(zip_prefix "${ZIP}" "/lib/cmake/OpenGL/OpenGLConfig.cmake")"
if [[ -z "${SDK_ROOT}" ]]; then
	echo "The archive is not a Mesa Switch 2 SDK (OpenGLConfig.cmake is missing)." >&2
	exit 1
fi
case "${SDK_ROOT}" in
	*portlibs/switch2) ;;
	*)
		echo "The archive is not a Switch 2 SDK (prefix ${SDK_ROOT}, expected .../portlibs/switch2)." >&2
		exit 1
		;;
esac

mkdir -p "$(dirname "${OUT}")"
WORK="$(mktemp -d "$(dirname "${OUT}")/switch2-mesa-sdk.prepare.XXXXXX")"
trap 'rm -rf -- "${WORK}"' EXIT

echo "Extracting the Switch 2 Mesa SDK from ${ZIP} ..."
"${BSDTAR}" -xf "${ZIP}" -C "${WORK}"
SDK_SOURCE="${WORK}/${SDK_ROOT}"
PREPARED="${WORK}/prepared"
mkdir -p "${PREPARED}"
cp -a "${SDK_SOURCE}/." "${PREPARED}/"
LIB="${PREPARED}/lib"

for required in libmesa_egl.a libmesa_gl.a libmesa_glapi.a libmesa_vulkan.a libEGL.a libvulkan.a; do
	if [[ ! -f "${LIB}/${required}" ]]; then
		echo "Switch 2 Mesa SDK file is missing: lib/${required}" >&2
		exit 1
	fi
done
for required in include/EGL/egl.h include/vulkan/vulkan.h; do
	if [[ ! -f "${PREPARED}/${required}" ]]; then
		echo "Switch 2 Mesa SDK file is missing: ${required}" >&2
		exit 1
	fi
done
if ! grep -q "mesa_vulkan" "${LIB}/libEGL.a" 2>/dev/null; then
	echo "lib/libEGL.a is not the Switch 2 SDK's linker script; refusing to rewrite it" >&2
	exit 1
fi

# The roots: the public EGL and GL entry points (Cemu's OpenGL renderer and SDL 2), the Vulkan ICD interface Cemu's
# loader starts from, NVK's loaderless entry points and the GPU teardown libounce calls at exit.
ROOTS="${WORK}/roots.txt"
{
	for archive in libmesa_egl.a libmesa_gl.a libmesa_glapi.a; do
		"${NM}" -g --defined-only "${LIB}/${archive}" 2>/dev/null |
			awk 'NF >= 3 && $2 ~ /^[TDBRW]$/ && $3 ~ /^(egl|gl)[A-Z]/ { print $3 }'
	done
	"${NM}" -g --defined-only "${LIB}/libmesa_vulkan.a" 2>/dev/null |
		awk 'NF >= 3 && $3 ~ /^(vk_icd|nvk_loaderless_)/ { print $3 } NF >= 3 && $3 == "__nx_gpu_exit" { print $3 }'
} | LC_ALL=C sort -u > "${ROOTS}"
if ! grep -qx "eglGetProcAddress" "${ROOTS}" || ! grep -qx "vk_icdGetInstanceProcAddr" "${ROOTS}"; then
	echo "The Switch 2 SDK archives lack the expected EGL or Vulkan ICD entry points" >&2
	exit 1
fi
echo "Merging EGL, GL and NVK from $(wc -l < "${ROOTS}") entry points ..."

MERGED="${WORK}/libmesa_merged.o"
LOCAL="${PREPARED}/libmesa_local.o"
UNDEFINED_ROOTS=()
while IFS= read -r symbol; do
	UNDEFINED_ROOTS+=("-u" "${symbol}")
done < "${ROOTS}"
# (single pass, in the order of the SDK's libEGL.a script: the same members as a normal link)
"${LD}" -r "${UNDEFINED_ROOTS[@]}" \
	"${LIB}/libmesa_egl.a" "${LIB}/libmesa_gl.a" "${LIB}/libmesa_glapi.a" "${LIB}/libmesa_vulkan.a" \
	-o "${MERGED}"

PUBLIC_VK_SYMBOLS="${WORK}/vulkan_public_symbols.txt"
"${NM}" -g --defined-only "${MERGED}" | awk '$NF ~ /^vk[A-Z]/ { print $NF }' | LC_ALL=C sort -u > "${PUBLIC_VK_SYMBOLS}"
if [[ ! -s "${PUBLIC_VK_SYMBOLS}" ]]; then
	echo "The merged Mesa object has no public Vulkan entry points to localize" >&2
	exit 1
fi
# Cemu provides writev (SwitchPosixCompat.cpp) and libounce provides posix_memalign (its mimalloc).
printf '%s\n' writev posix_memalign >> "${PUBLIC_VK_SYMBOLS}"
LC_ALL=C sort -u -o "${PUBLIC_VK_SYMBOLS}" "${PUBLIC_VK_SYMBOLS}"
"${OBJCOPY}" --localize-symbols="${PUBLIC_VK_SYMBOLS}" "${MERGED}" "${LOCAL}"

if "${NM}" -g --defined-only "${LOCAL}" | awk '$NF ~ /^vk[A-Z]/ { found = 1 } END { exit !found }'; then
	echo "The localized Mesa object still exports a public Vulkan entry point" >&2
	exit 1
fi
for symbol in \
	vk_icdGetInstanceProcAddr \
	vk_icdNegotiateLoaderICDInterfaceVersion \
	nvk_loaderless_GetInstanceProcAddr \
	nvk_loaderless_GetDeviceProcAddr \
	eglGetDisplay \
	eglGetProcAddress; do
	if ! "${NM}" -g --defined-only "${LOCAL}" |
		awk -v expected="${symbol}" '$NF == expected { found = 1 } END { exit !found }'; then
		echo "The localized Mesa object does not export ${symbol}" >&2
		exit 1
	fi
done

# libEGL.a and libvulkan.a now name the merged object's archive (the C++, math and compression libraries after it,
# as in the SDK's scripts). libvulkan_local.o is the object the Cemu core links whole (Vulkan, and uam2rt's Mesa).
"${AR}" rcs "${LIB}/libmesa_local.a" "${LOCAL}"
printf 'INPUT(-lmesa_local -lstdc++ -lm -lzstd -lexpat -lz)\n' > "${LIB}/libEGL.a"
printf 'INPUT(-lmesa_local -lstdc++ -lm -lzstd -lexpat -lz)\n' > "${LIB}/libvulkan.a"
cp "${LOCAL}" "${PREPARED}/libvulkan_local.o"
cp "${LIB}/libmesa_local.a" "${PREPARED}/libvulkan_local.a"
rm -f "${LOCAL}"
mkdir -p "${PREPARED}/share"
printf '%s' "${STAMP_VALUE}" > "${PREPARED}/share/cemu-switch2-sdk.stamp"
basename "${ZIP}" > "${PREPARED}/share/cemu-switch2-sdk.source"

OLD="${OUT}.old.$$"
if [[ -e "${OUT}" ]]; then
	mv "${OUT}" "${OLD}"
fi
mv "${PREPARED}" "${OUT}"
if [[ -e "${OLD}" ]]; then
	rm -rf -- "${OLD}"
fi

echo "Prepared the Switch 2 Mesa SDK at ${OUT}"
echo "Merged Mesa object (EGL, GL, NVK; vk* localized): ${OUT}/libvulkan_local.o"
echo "uam2rt: ${OUT}/lib/cmake/uam2rt"
