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
# loaded from System32 at Init (idD3D12Library), so no d3d12, dxgi or d3dcompiler import library is linked.
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

# Links the engine into an executable: its objects, the system libraries, the 16 MB stack the
# single-threaded frame needs, and the delay-loaded FFmpeg DLLs copied next to the exe.
function(d3bfg_link_engine tgt)
	target_link_libraries(${tgt} PRIVATE
		engine
		idLib
		Game-d3xp
		external
		doomclassic
		timidity
		xaudio2 xinput dbghelp dinput8 dsound dxguid iphlpapi
		odbc32 odbccp32 opengl32 winmm wsock32
	)
	target_link_options(${tgt} PRIVATE
		/STACK:16777216,16777216
		/DEBUG
		$<$<CONFIG:Debug>:/MAP>
		$<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>
	)
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
# and texture stores, the PSO factory, the debug groups, the GPU timers, the screen copies and the draw recorder on a WARP
# device (the texture border, decoding, timer, copy and draw tests also on the GPU), and the HLSL emitter and FXC over
# base/renderprogs against neo/tests/d3d12_golden
#   bin/<config>/d3d12Tests.exe [filter]
add_executable(d3d12Tests
	tests/d3d12_tests.cpp
	tests/d3d12_test_stubs.cpp
	renderer/D3D12/d3d12_Buffers.cpp
	renderer/D3D12/d3d12_Copies.cpp
	renderer/D3D12/d3d12_Descriptors.cpp
	renderer/D3D12/d3d12_Device.cpp
	renderer/D3D12/d3d12_Draw.cpp
	renderer/D3D12/d3d12_Formats.cpp
	renderer/D3D12/d3d12_HlslEmitter.cpp
	renderer/D3D12/d3d12_InternalPasses.cpp
	renderer/D3D12/d3d12_Memory.cpp
	renderer/D3D12/d3d12_Output.cpp
	renderer/D3D12/d3d12_Pipelines.cpp
	renderer/D3D12/d3d12_Programs.cpp
	renderer/D3D12/d3d12_RingAllocator.cpp
	renderer/D3D12/d3d12_ShaderCompiler.cpp
	renderer/D3D12/d3d12_StateMap.cpp
	renderer/D3D12/d3d12_Textures.cpp
	renderer/D3D12/d3d12_Timers.cpp
	renderer/DXT/DXTDecoder.cpp
	renderer/RenderProgSource.cpp
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
target_link_libraries(d3d12Tests PRIVATE idLib winmm dbghelp dxguid)
target_link_options(d3d12Tests PRIVATE /DEBUG)
set_target_properties(d3d12Tests PROPERTIES FOLDER "tests")
add_test(NAME d3d12Tests COMMAND d3d12Tests)

# renderApiLeaks: no OpenGL name outside neo/renderer/OpenGL/ and the two WGL files, no D3D12 or DXGI
# name outside neo/renderer/D3D12/
add_test(NAME renderApiLeaks
	COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tools/check-render-api-leaks.ps1")

# editorModules: editor/ui/ includes no engine header, editor/bridge/ no Qt header
add_test(NAME editorModules
	COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tools/check-editor-modules.ps1")
