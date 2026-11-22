add_library(CemuAudio
	IAudioAPI.cpp
	IAudioAPI.h
	IAudioInputAPI.cpp
	IAudioInputAPI.h
)

set_property(TARGET CemuAudio PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

cemu_use_precompiled_header(CemuAudio)

if(WIN32)
	target_sources(CemuAudio PRIVATE
		DirectSoundAPI.cpp
		DirectSoundAPI.h
		XAudio2API.cpp
		XAudio2API.h
		XAudio27API.cpp
		XAudio27API.h
	)
endif()

if(ENABLE_CUBEB)
	target_sources(CemuAudio PRIVATE
		CubebAPI.cpp
		CubebAPI.h
		CubebInputAPI.cpp
		CubebInputAPI.h
	)
	#add_compile_definitions(HAS_CUBEB)
endif()

if(PLATFORM_SWITCH)
	target_sources(CemuAudio PRIVATE
		SwitchAudioAPI.h
		${CMAKE_SOURCE_DIR}/src/platform/switch/SwitchAudioAPI.cpp
		AudrenAPI.h
		${CMAKE_SOURCE_DIR}/src/platform/switch/AudrenAPI.cpp
		${CMAKE_SOURCE_DIR}/src/platform/switch/SwitchAudren.h
		${CMAKE_SOURCE_DIR}/src/platform/switch/SwitchAudren.cpp
	)
endif()

target_include_directories(CemuAudio PUBLIC "../")

target_link_libraries(CemuAudio PRIVATE
	CemuCommon
	CemuGui
)

if(ENABLE_CUBEB)
	# PUBLIC because cubeb.h/cubeb.h is included in CubebAPI.h
	target_link_libraries(CemuAudio PUBLIC cubeb::cubeb)
endif()
