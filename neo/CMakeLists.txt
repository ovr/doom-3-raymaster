# Doom 3 BFG engine, game and tools

include(sources.cmake)

set(D3BFG_PCH "${CMAKE_CURRENT_SOURCE_DIR}/idlib/precompiled.h")

# ---------------------------------------------------------------------------
# idLib
#
# idLib, Game-d3xp and external are OBJECT libraries: the old solution linked
# their object files directly into the executable (UseLibraryDependencyInputs),
# which keeps self-registering globals (idCVar, idTypeInfo, idEventDef, ...)
# that a static .lib would let the linker drop.
# ---------------------------------------------------------------------------
add_library(idLib OBJECT ${IDLIB_SOURCES})
d3bfg_neo_common(idLib)
d3bfg_configure_target(idLib WARNING_LEVEL 4 WITH_INLINES WERROR)
target_compile_definitions(idLib PRIVATE __IDLIB__ _MBCS)
target_precompile_headers(idLib PRIVATE "${D3BFG_PCH}")
set_source_files_properties(
	idlib/geometry/RenderMatrix.cpp
	idlib/SoftwareCache.cpp
	TARGET_DIRECTORY idLib
	PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

# ---------------------------------------------------------------------------
# external: libjpeg-6 and zlib
# ---------------------------------------------------------------------------
add_library(external OBJECT ${EXTERNAL_SOURCES})
d3bfg_neo_common(external)
d3bfg_configure_target(external WARNING_LEVEL 3)
target_compile_definitions(external PRIVATE WIN32 _LIB UNICODE _UNICODE)
target_include_directories(external PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/renderer/jpeg-6")

# ---------------------------------------------------------------------------
# Game-d3xp
# ---------------------------------------------------------------------------
add_library(Game-d3xp OBJECT ${GAME_D3XP_SOURCES})
d3bfg_neo_common(Game-d3xp)
d3bfg_configure_target(Game-d3xp WARNING_LEVEL 4 WITH_INLINES WERROR)
target_compile_definitions(Game-d3xp PRIVATE
	__DOOM__ _D3XP CTF _MBCS
	$<$<CONFIG:Retail>:ID_RETAIL>
)
target_precompile_headers(Game-d3xp PRIVATE "${D3BFG_PCH}")

set_target_properties(idLib external Game-d3xp PROPERTIES FOLDER "neo")

# ---------------------------------------------------------------------------
# Doom3BFG executable
# ---------------------------------------------------------------------------
add_executable(Doom3BFG WIN32
	${DOOMEXE_SOURCES}
	sys/win32/rc/doom.rc
	sys/win32/rc/doom3bfg.manifest
)
d3bfg_neo_common(Doom3BFG)
d3bfg_configure_target(Doom3BFG WARNING_LEVEL 4 WITH_INLINES WERROR)
target_compile_definitions(Doom3BFG PRIVATE
	__DOOM__ _MBCS
	$<$<CONFIG:Retail>:ID_RETAIL>
)
target_precompile_headers(Doom3BFG PRIVATE "${D3BFG_PCH}")
set_source_files_properties(
	renderer/jobs/ShadowShared.cpp
	renderer/jobs/prelightshadowvolume/PreLightShadowVolume.cpp
	renderer/jobs/staticshadowvolume/StaticShadowVolume.cpp
	renderer/jobs/dynamicshadowvolume/DynamicShadowVolume.cpp
	TARGET_DIRECTORY Doom3BFG
	PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

target_link_libraries(Doom3BFG PRIVATE
	idLib
	Game-d3xp
	external
	doomclassic
	timidity
	xaudio2 xinput dbghelp dinput8 dsound dxguid iphlpapi
	odbc32 odbccp32 opengl32 winmm wsock32
)
target_link_options(Doom3BFG PRIVATE
	/STACK:16777216,16777216
	/LARGEADDRESSAWARE
	/DEBUG
	$<$<CONFIG:Debug>:/MAP>
	$<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>
)
set_target_properties(Doom3BFG PROPERTIES FOLDER "neo")
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT Doom3BFG)

# FFmpeg (optional, tools/ffmpeg/build-ffmpeg-win32.cmd) plays the Bink videos, without it they
# stay black. The DLLs are delay-loaded and copied next to the exe, which starts without them.
set(D3BFG_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/external/ffmpeg-win32" CACHE PATH
	"FFmpeg for the Bink videos (include/, bin/*.dll, bin/ or lib/*.lib); empty or missing disables them")

set(D3BFG_FFMPEG_FOUND OFF)
set(_ffmpeg_implibs "")
set(_ffmpeg_dlls "")
if(D3BFG_FFMPEG_ROOT AND EXISTS "${D3BFG_FFMPEG_ROOT}/include/libavformat/avformat.h")
	set(D3BFG_FFMPEG_FOUND ON)
	foreach(_lib avformat avcodec avutil)
		set(_implib "")
		foreach(_dir lib bin)
			if(EXISTS "${D3BFG_FFMPEG_ROOT}/${_dir}/${_lib}.lib")
				set(_implib "${D3BFG_FFMPEG_ROOT}/${_dir}/${_lib}.lib")
				break()
			endif()
		endforeach()
		file(GLOB _dll "${D3BFG_FFMPEG_ROOT}/bin/${_lib}-*.dll")
		list(LENGTH _dll _num_dlls)
		if(NOT _implib OR NOT _num_dlls EQUAL 1)
			set(D3BFG_FFMPEG_FOUND OFF)
			break()
		endif()
		list(APPEND _ffmpeg_implibs "${_implib}")
		list(APPEND _ffmpeg_dlls "${_dll}")
	endforeach()
endif()

if(D3BFG_FFMPEG_FOUND)
	message(STATUS "FFmpeg: ${D3BFG_FFMPEG_ROOT} (Bink videos on)")
	target_compile_definitions(Doom3BFG PRIVATE USE_FFMPEG)
	target_include_directories(Doom3BFG SYSTEM PRIVATE "${D3BFG_FFMPEG_ROOT}/include")
	target_link_libraries(Doom3BFG PRIVATE ${_ffmpeg_implibs} delayimp)
	foreach(_dll IN LISTS _ffmpeg_dlls)
		get_filename_component(_dll_name "${_dll}" NAME)
		target_link_options(Doom3BFG PRIVATE "/DELAYLOAD:${_dll_name}")
	endforeach()
	add_custom_command(TARGET Doom3BFG POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ffmpeg_dlls} "$<TARGET_FILE_DIR:Doom3BFG>"
		COMMENT "Copying the FFmpeg DLLs next to Doom3BFG.exe"
		VERBATIM
	)
else()
	message(STATUS "FFmpeg not found in '${D3BFG_FFMPEG_ROOT}': Bink videos off "
		"(build it with tools/ffmpeg/build-ffmpeg-win32.cmd or set D3BFG_FFMPEG_ROOT)")
endif()

# ---------------------------------------------------------------------------
# amplitude (standalone console tool)
# ---------------------------------------------------------------------------
add_executable(amplitude ${AMPLITUDE_SOURCES})
d3bfg_configure_target(amplitude WARNING_LEVEL 3)
target_compile_definitions(amplitude PRIVATE WIN32 _CONSOLE UNICODE _UNICODE)
set_target_properties(amplitude PROPERTIES FOLDER "tools")

# ---------------------------------------------------------------------------
# idlibTests: unit tests for the idLib math used by the physics code
#   cmake --build --preset release --target idlibTests
#   ctest --preset release            (or run bin/<config>/idlibTests.exe [filter])
# ---------------------------------------------------------------------------
add_executable(idlibTests
	tests/idlib_tests.cpp
	tests/idlib_test_stubs.cpp
	tests/lcp_generic.cpp
)
d3bfg_neo_common(idlibTests)
d3bfg_configure_target(idlibTests WARNING_LEVEL 3)
target_compile_definitions(idlibTests PRIVATE _MBCS _CONSOLE)
target_link_libraries(idlibTests PRIVATE idLib winmm dbghelp)
target_link_options(idlibTests PRIVATE /LARGEADDRESSAWARE /DEBUG)
set_target_properties(idlibTests PROPERTIES FOLDER "tests")
add_test(NAME idlibTests COMMAND idlibTests)
