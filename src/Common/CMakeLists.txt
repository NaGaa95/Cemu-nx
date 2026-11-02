add_library(CemuCommon 
  betype.h
  CafeStaticVar.cpp
  CafeStaticVar.h
  cpu_features.cpp
  cpu_features.h
  enumFlags.h
  ExceptionHandler/ExceptionHandler.cpp
  ExceptionHandler/ExceptionHandler.h
  FileStream.h
  GLInclude/glext.h
  GLInclude/glFunctions.h
  GLInclude/GLInclude.h
  GLInclude/glxext.h
  GLInclude/khrplatform.h
  GLInclude/wglext.h
  MemPtr.h
  platform.h
  precompiled.cpp
  precompiled.h
  socket.h
  StackAllocator.h
  SysAllocator.cpp
  SysAllocator.h
  CafeString.h
  version.h
)

set_property(TARGET CemuCommon PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(WIN32)
	target_sources(CemuCommon PRIVATE
		windows/platform.cpp
		windows/platform.h
		windows/FileStream_win32.cpp
		ExceptionHandler/ExceptionHandler_win32.cpp
	)
else()
target_sources(CemuCommon
PRIVATE
    unix/date.h
    unix/fast_float.h
    unix/platform.cpp
    unix/platform.h
    unix/FileStream_unix.cpp
    unix/FileStream_unix.h
)
if(PLATFORM_SWITCH)
	target_sources(CemuCommon PRIVATE ExceptionHandler/ExceptionHandler_switch.cpp)
	# Thread affinity policy: referenced from CemuCafe and CemuAudio, so it has
	# to live in a library both of them link rather than in the executable.
	target_sources(CemuCommon PRIVATE ${CMAKE_SOURCE_DIR}/src/platform/switch/SwitchThread.cpp)
	target_sources(CemuCommon PRIVATE ${CMAKE_SOURCE_DIR}/src/platform/switch/SwitchMemory.cpp)
	# Keep the libnx exception-stack override out of GCC LTO.
	set_property(SOURCE ExceptionHandler/ExceptionHandler_switch.cpp APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
else()
	target_sources(CemuCommon PRIVATE ExceptionHandler/ExceptionHandler_posix.cpp)
endif()
endif()

if(LINUX)
	target_sources(CemuCommon PRIVATE
		ExceptionHandler/ELFSymbolTable.cpp
		ExceptionHandler/ELFSymbolTable.h
	)
endif()

# CemuCommon always builts the precompiled header
if(PLATFORM_SWITCH)
	# devkitA64 cannot consume the MSYS paths in a generated PCH.
	target_compile_options(CemuCommon PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/precompiled.h>")
else()
	target_precompile_headers(CemuCommon PRIVATE precompiled.h)
endif()
target_include_directories(CemuCommon PUBLIC "../")

target_link_libraries(CemuCommon PRIVATE
	glm::glm
)

if (UNIX AND NOT APPLE)
	target_link_libraries(CemuCommon PRIVATE X11::X11 X11::Xrender X11::Xutil)
endif()

# PUBLIC because:
# - boost/nowide/convert.hpp is included in precompiled.h
# - boost/predef/os.h is included in platform.h
# - fmt/core.h is included in precompiled.h
target_link_libraries(CemuCommon PUBLIC
	Threads::Threads # necessary because some targets may need -pthreads
	Boost::nowide
	Boost::headers
	fmt::fmt
)
