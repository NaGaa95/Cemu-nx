# Switch 2 (kaiHEN) toolchain: devkitA64 with ounce-devkit (libounce, drako3d, kaiHEN). libounce's Switch2.cmake sets
# NINTENDO_SWITCH2, __SWITCH2__ and the Cortex-A78C flags; dist/switch/build-switch2.sh configures it.
if(DEFINED ENV{DEVKITPRO})
	set(DEVKITPRO $ENV{DEVKITPRO})
else()
	set(DEVKITPRO "/opt/devkitpro")
endif()

if(NOT EXISTS "${DEVKITPRO}/libounce/cmake/Switch2.cmake")
	message(FATAL_ERROR "No Switch 2 devkit (libounce) in ${DEVKITPRO}: install https://github.com/ouncebrew/ounce-devkit")
endif()

include("${DEVKITPRO}/libounce/cmake/Switch2.cmake")

set(PLATFORM_SWITCH ON CACHE BOOL "Target the Nintendo Switch (libnx)" FORCE)
set(CEMU_ARCHITECTURE "aarch64" CACHE STRING "" FORCE)

add_compile_definitions(__SWITCH__ NINTENDO_SWITCH)

add_compile_definitions(BOOST_ASIO_DISABLE_SERIAL_PORT)

add_compile_options(
	-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
	-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=.
	-fno-ident)

set(ENABLE_VCPKG        OFF CACHE BOOL "" FORCE)
set(ENABLE_WXWIDGETS    OFF CACHE BOOL "" FORCE)
if(CEMU_SWITCH_DEKO3D_RUNTIME)
    set(ENABLE_DEKO3D ON CACHE BOOL "" FORCE)
    set(ENABLE_OPENGL OFF CACHE BOOL "" FORCE)
    set(ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
else()
    set(ENABLE_DEKO3D OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPENGL ON CACHE BOOL "" FORCE)
    set(ENABLE_VULKAN ON CACHE BOOL "" FORCE)
endif()
set(ENABLE_METAL        OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL          OFF CACHE BOOL "" FORCE)
set(ENABLE_CUBEB        OFF CACHE BOOL "" FORCE)
set(ENABLE_DISCORD_RPC  OFF CACHE BOOL "" FORCE)
set(ENABLE_HIDAPI       OFF CACHE BOOL "" FORCE)
set(ENABLE_BLUEZ        OFF CACHE BOOL "" FORCE)
set(ENABLE_WAYLAND      OFF CACHE BOOL "" FORCE)
set(ENABLE_FERAL_GAMEMODE OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBUSB       OFF CACHE BOOL "" FORCE)
set(ALLOW_PORTABLE      OFF CACHE BOOL "" FORCE)

# The bundled GCC profiles were trained on the Switch build (Cortex-A57).

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON CACHE BOOL "" FORCE)
