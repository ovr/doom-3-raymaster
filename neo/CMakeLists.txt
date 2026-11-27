# Doom 3 BFG engine, game and tools

include(sources.cmake)

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
# engine: everything but WinMain, an OBJECT library for the same reason as idLib. Every executable
# that hosts the engine (the game, the editor) links these objects through d3bfg_link_engine.
# ---------------------------------------------------------------------------
add_library(engine OBJECT ${ENGINE_SOURCES})
d3bfg_neo_common(engine)
d3bfg_configure_target(engine WARNING_LEVEL 4 WITH_INLINES WERROR)
target_compile_definitions(engine PRIVATE
	__DOOM__ _MBCS
	$<$<CONFIG:Retail>:ID_RETAIL>
)
target_precompile_headers(engine PRIVATE "${D3BFG_PCH}")
set_source_files_properties(
	renderer/jobs/ShadowShared.cpp
	renderer/jobs/prelightshadowvolume/PreLightShadowVolume.cpp
	renderer/jobs/staticshadowvolume/StaticShadowVolume.cpp
	renderer/jobs/dynamicshadowvolume/DynamicShadowVolume.cpp
	TARGET_DIRECTORY engine
	PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)
# renderer/D3D12/ is new code without warnings: /WX in every config, with no build option. Its DLLs are
# loaded at Init (idD3D12Library), so no d3d12, dxgi or dxcompiler import library is linked.
set_source_files_properties(${D3D12_SOURCES} TARGET_DIRECTORY engine PROPERTIES COMPILE_OPTIONS /WX)
set_target_properties(engine PROPERTIES FOLDER "neo")

# FFmpeg (optional, tools/ffmpeg/build-ffmpeg-x64.cmd) plays the Bink videos, without it they
# stay black. The DLLs are delay-loaded and copied next to the exe, which starts without them.
set(D3BFG_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/external/ffmpeg-x64" CACHE PATH
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
	target_compile_definitions(engine PRIVATE USE_FFMPEG)
	target_include_directories(engine SYSTEM PRIVATE "${D3BFG_FFMPEG_ROOT}/include")
else()
	message(STATUS "FFmpeg not found in '${D3BFG_FFMPEG_ROOT}': Bink videos off "
		"(build it with tools/ffmpeg/build-ffmpeg-x64.cmd or set D3BFG_FFMPEG_ROOT)")
endif()
# global, so that d3bfg_link_engine also works from other directories (editor/)
set_property(GLOBAL PROPERTY D3BFG_FFMPEG_IMPLIBS "${_ffmpeg_implibs}")
set_property(GLOBAL PROPERTY D3BFG_FFMPEG_DLLS "${_ffmpeg_dlls}")
set_property(GLOBAL PROPERTY D3BFG_FFMPEG_FOUND "${D3BFG_FFMPEG_FOUND}")

# DXC, the Agility SDK and WARP (tools/d3d12/install-d3d12-sdk.ps1). The D3D12 backend compiles with the Agility
# headers and DXC's dxcapi.h; dxcompiler.dll, dxil.dll and the Agility runtime ship next to the exe.
# D3BFG_D3D12_SDK_ROOT is set in the root CMakeLists.txt; dxc.exe compiles NRD's shaders at build time.
foreach(_file dxc/inc/dxcapi.h dxc/bin/x64/dxc.exe dxc/bin/x64/dxcompiler.dll dxc/bin/x64/dxil.dll agility/include/d3d12.h
		agility/bin/x64/D3D12Core.dll agility/bin/x64/d3d12SDKLayers.dll warp/bin/x64/d3d10warp.dll)
	if(NOT EXISTS "${D3BFG_D3D12_SDK_ROOT}/${_file}")
		message(FATAL_ERROR "${D3BFG_D3D12_SDK_ROOT}/${_file} is missing: run tools/d3d12/install-d3d12-sdk.ps1 "
			"(or point D3BFG_D3D12_SDK_ROOT at a copy), then configure again")
	endif()
endforeach()
set(D3BFG_D3D12_INCLUDES "${D3BFG_D3D12_SDK_ROOT}/agility/include" "${D3BFG_D3D12_SDK_ROOT}/dxc/inc")
# before the Windows SDK, whose older d3d12.h would win otherwise
target_include_directories(engine SYSTEM BEFORE PRIVATE ${D3BFG_D3D12_INCLUDES})

# NRD (third_party/CMakeLists.txt), the denoiser of the ray traced passes: its headers come in as system includes
if(D3BFG_NRD)
	target_compile_definitions(engine PRIVATE USE_NRD)
	target_link_libraries(engine PRIVATE NRD)
endif()

# DXC next to the exe and the Agility runtime in D3D12\ beside it (D3D12SDKPath, d3d12_Agility.cpp). WARP with
# feature level 12_2 only for the tests: its license allows testing, not shipping it with the game.
function(d3bfg_copy_d3d12_runtime tgt)
	cmake_parse_arguments(PARSE_ARGV 1 ARG "WITH_WARP" "" "")
	set(_sdk "${D3BFG_D3D12_SDK_ROOT}")
	set(_beside "${_sdk}/dxc/bin/x64/dxcompiler.dll" "${_sdk}/dxc/bin/x64/dxil.dll")
	if(ARG_WITH_WARP)
		list(APPEND _beside "${_sdk}/warp/bin/x64/d3d10warp.dll")
	endif()
	add_custom_command(TARGET ${tgt} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_beside} "$<TARGET_FILE_DIR:${tgt}>"
		COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${tgt}>/D3D12"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sdk}/agility/bin/x64/D3D12Core.dll"
			"${_sdk}/agility/bin/x64/d3d12SDKLayers.dll" "$<TARGET_FILE_DIR:${tgt}>/D3D12"
		COMMENT "Copying DXC and the Agility runtime next to ${tgt}"
		VERBATIM
	)
endfunction()

# Links the engine into an executable: its objects, the map compilers (editor/compilers), the system libraries, the 16 MB stack the
# single-threaded frame needs, the delay-loaded FFmpeg DLLs, DXC and the Agility runtime copied next to the exe.
function(d3bfg_link_engine tgt)
	d3bfg_copy_d3d12_runtime(${tgt})
	target_link_libraries(${tgt} PRIVATE
		engine
		MapCompilers
		idLib
		Game-d3xp
		external
		doomclassic
		timidity
		xaudio2 xinput dbghelp dinput8 dsound dxguid iphlpapi
		odbc32 odbccp32 winmm wsock32
	)
	target_link_options(${tgt} PRIVATE
		/STACK:16777216,16777216
		/DEBUG
		$<$<CONFIG:Debug>:/MAP>
		$<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>
	)
	if(D3BFG_NRD)
		target_link_libraries(${tgt} PRIVATE NRD)
	endif()
	get_property(_found GLOBAL PROPERTY D3BFG_FFMPEG_FOUND)
	if(_found)
		get_property(_implibs GLOBAL PROPERTY D3BFG_FFMPEG_IMPLIBS)
		get_property(_dlls GLOBAL PROPERTY D3BFG_FFMPEG_DLLS)
		target_link_libraries(${tgt} PRIVATE ${_implibs} delayimp)
		foreach(_dll IN LISTS _dlls)
			get_filename_component(_dll_name "${_dll}" NAME)
			target_link_options(${tgt} PRIVATE "/DELAYLOAD:${_dll_name}")
		endforeach()
		add_custom_command(TARGET ${tgt} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dlls} "$<TARGET_FILE_DIR:${tgt}>"
			COMMENT "Copying the FFmpeg DLLs next to ${tgt}"
			VERBATIM
		)
	endif()
endfunction()

# ---------------------------------------------------------------------------
# Doom3BFG executable: WinMain, the icon and the manifest around the engine
# ---------------------------------------------------------------------------
add_executable(Doom3BFG WIN32
	${DOOMEXE_SOURCES}
	sys/win32/rc/doom.rc
	sys/win32/rc/doom3bfg.manifest
)
d3bfg_configure_target(Doom3BFG WARNING_LEVEL 4 WERROR)
d3bfg_link_engine(Doom3BFG)
set_target_properties(Doom3BFG PROPERTIES FOLDER "neo")
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT Doom3BFG)

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

# d3d12Tests: the ring, the handle table, the format, sampler and pipeline state tables, the uploader, buffer
# and texture stores, the PSO factory, the debug groups, the GPU timers, the screen copies, the draw recorder and the render graph on a WARP
# device (the texture border, decoding, timer, copy and draw tests also on the GPU), DXC and a bindless draw on SM 6.6,
# and base/shaders against the program interfaces in neo/tests/d3d12_golden/renderProgInfo.txt
#   bin/<config>/d3d12Tests.exe [filter]
add_executable(d3d12Tests
	tests/d3d12_tests.cpp
	tests/d3d12_test_stubs.cpp
	renderer/D3D12/d3d12_Agility.cpp
	renderer/D3D12/d3d12_Buffers.cpp
	renderer/D3D12/d3d12_Copies.cpp
	renderer/D3D12/d3d12_Descriptors.cpp
	renderer/D3D12/d3d12_Device.cpp
	renderer/D3D12/d3d12_Draw.cpp
	renderer/D3D12/d3d12_Dxc.cpp
	renderer/D3D12/d3d12_Formats.cpp
	renderer/D3D12/d3d12_GpuScene.cpp
	renderer/D3D12/d3d12_Hdr.cpp
	renderer/D3D12/d3d12_HlslCompiler.cpp
	renderer/D3D12/d3d12_InternalPasses.cpp
	renderer/D3D12/d3d12_Memory.cpp
	renderer/D3D12/d3d12_Output.cpp
	renderer/D3D12/d3d12_Pipelines.cpp
	renderer/D3D12/d3d12_Programs.cpp
	renderer/D3D12/d3d12_RayTracing.cpp
	renderer/D3D12/d3d12_RenderGraph.cpp
	renderer/D3D12/d3d12_RingAllocator.cpp
	renderer/D3D12/d3d12_RtHistory.cpp
	renderer/D3D12/d3d12_RtPasses.cpp
	renderer/D3D12/d3d12_ShaderCompiler.cpp
	renderer/D3D12/d3d12_StateMap.cpp
	renderer/D3D12/d3d12_Textures.cpp
	renderer/D3D12/d3d12_Timers.cpp
	renderer/DXT/DXTDecoder.cpp
	renderer/RenderProgTable.cpp
)
d3bfg_neo_common(d3d12Tests)
# /W3 like idlibTests: at /W4 the PCH of old idLib headers warns again. Doom3BFG builds the D3D12 files at /W4;
# /WX per file, as for D3D12_SOURCES above, whose property this directory shares.
d3bfg_configure_target(d3d12Tests WARNING_LEVEL 3 WITH_INLINES)
set_source_files_properties(tests/d3d12_tests.cpp tests/d3d12_test_stubs.cpp PROPERTIES COMPILE_OPTIONS /WX)
target_compile_definitions(d3d12Tests PRIVATE
	__DOOM__ _MBCS _CONSOLE
	D3D12_TESTS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
	$<$<CONFIG:Retail>:ID_RETAIL>
)
target_precompile_headers(d3d12Tests PRIVATE "${D3BFG_PCH}")
target_include_directories(d3d12Tests SYSTEM BEFORE PRIVATE ${D3BFG_D3D12_INCLUDES})
target_link_libraries(d3d12Tests PRIVATE idLib winmm dbghelp dxguid)
if(D3BFG_NRD)
	target_compile_definitions(d3d12Tests PRIVATE USE_NRD)
	target_link_libraries(d3d12Tests PRIVATE NRD)
endif()
d3bfg_copy_d3d12_runtime(d3d12Tests WITH_WARP)
target_link_options(d3d12Tests PRIVATE /DEBUG)
set_target_properties(d3d12Tests PROPERTIES FOLDER "tests")
add_test(NAME d3d12Tests COMMAND d3d12Tests)

# renderApiLeaks: no OpenGL name anywhere, no D3D12 or DXGI name outside neo/renderer/D3D12/
add_test(NAME renderApiLeaks
	COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tools/check-render-api-leaks.ps1")

# editorModules: editor/ui/ includes no engine header, editor/bridge/ no Qt header
add_test(NAME editorModules
	COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tools/check-editor-modules.ps1")
