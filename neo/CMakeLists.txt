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
	xaudio2 xinput dbghelp dinput8 dsound dxguid glu32 iphlpapi
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
