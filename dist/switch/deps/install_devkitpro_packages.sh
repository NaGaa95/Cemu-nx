#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/common.sh"
parse_check_only "$@"

DEVKIT_PACKAGES=(
	devkitA64
	libnx
	deko3d
	switch-tools
	switch-cmake
	switch-pkg-config
	switch-zlib
	switch-libzstd
	switch-libpng
	switch-freetype
	switch-mbedtls
	switch-curl
	switch-ffmpeg
	switch-dav1d
	switch-libelf
	switch-libdrm_nouveau
	switch-mesa
	switch-sdl2
	switch-sdl2_image
	switch-sdl2_ttf
	switch-libjpeg-turbo
)
HOST_PACKAGES=(git cmake ninja make gcc curl tar bzip2 bison flex python python-mako)
HOST_COMMANDS=(git cmake ninja make g++ curl tar sha256sum bison flex python3)

find_pacman() {
	if [[ -n "${DKP_PACMAN:-}" && -x "${DKP_PACMAN}" ]]; then
		printf '%s\n' "${DKP_PACMAN}"
	elif command -v dkp-pacman >/dev/null 2>&1; then
		command -v dkp-pacman
	elif [[ -x "${DEVKITPRO}/pacman/bin/pacman" ]]; then
		printf '%s\n' "${DEVKITPRO}/pacman/bin/pacman"
	elif command -v pacman >/dev/null 2>&1; then
		command -v pacman
	else
		return 1
	fi
}

PACMAN_BIN="$(find_pacman)" || die "devkitPro pacman was not found"
declare -A INSTALLED_PACKAGES=()
while IFS= read -r package; do
	INSTALLED_PACKAGES["${package}"]=1
done < <("${PACMAN_BIN}" -Qq)

MISSING=()
for package in "${DEVKIT_PACKAGES[@]}"; do
	[[ -n "${INSTALLED_PACKAGES[${package}]:-}" ]] || MISSING+=("${package}")
done

UNAME="$(uname -s)"
if [[ "${UNAME}" == MSYS* || "${UNAME}" == MINGW* || "${UNAME}" == CYGWIN* ]]; then
	for package in "${HOST_PACKAGES[@]}"; do
		[[ -n "${INSTALLED_PACKAGES[${package}]:-}" ]] || MISSING+=("${package}")
	done
fi

if (( CHECK_ONLY )); then
	FAILED=0
	if (( ${#MISSING[@]} )); then
		echo "Missing packages: ${MISSING[*]}" >&2
		FAILED=1
	fi
	for command_name in "${HOST_COMMANDS[@]}"; do
		if ! command -v "${command_name}" >/dev/null 2>&1; then
			echo "Missing host command: ${command_name}" >&2
			FAILED=1
		fi
	done
	(( FAILED == 0 )) || exit 1
	echo "Ready: devkitPro and host packages"
	exit 0
fi

if (( ${#MISSING[@]} )); then
	PACMAN_COMMAND=("${PACMAN_BIN}")
	if [[ "${UNAME}" != MSYS* && "${UNAME}" != MINGW* && "${UNAME}" != CYGWIN* && "${EUID}" -ne 0 ]]; then
		command -v sudo >/dev/null 2>&1 || die "run this script as root or install sudo"
		PACMAN_COMMAND=(sudo -E "${PACMAN_BIN}")
	fi
	"${PACMAN_COMMAND[@]}" -S --needed "${MISSING[@]}"
fi

for command_name in "${HOST_COMMANDS[@]}"; do
	command -v "${command_name}" >/dev/null 2>&1 ||
		die "install the host command '${command_name}' with your operating-system package manager"
done
echo "Ready: devkitPro and host packages"
