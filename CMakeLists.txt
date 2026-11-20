cmake_minimum_required(VERSION 3.21)

project(Doom3BFG LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Platform checks: the code base is Windows / MSVC / x64 only.
# ---------------------------------------------------------------------------
if(NOT MSVC)
	message(FATAL_ERROR "Doom 3 BFG can only be built with MSVC.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR
		"Doom 3 BFG must be built as 64-bit (x64). "
		"Use the Visual Studio generator with '-A x64' or an x64 Native Tools prompt.")
endif()

# ---------------------------------------------------------------------------
# Configurations: Debug, Release, Retail (Retail = Release + ID_RETAIL)
# ---------------------------------------------------------------------------
get_property(D3BFG_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(D3BFG_MULTI_CONFIG)
	set(CMAKE_CONFIGURATION_TYPES Debug Release Retail CACHE STRING "" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type: Debug, Release or Retail" FORCE)
endif()

# Optimization / debug flags are set per target below, keep the CMake
# defaults minimal so they don't conflict.
foreach(lang C CXX)
	set(CMAKE_${lang}_FLAGS_DEBUG "")
	set(CMAKE_${lang}_FLAGS_RELEASE "")
	set(CMAKE_${lang}_FLAGS_RETAIL "")
endforeach()
foreach(kind EXE SHARED MODULE STATIC)
	set(CMAKE_${kind}_LINKER_FLAGS_RETAIL "${CMAKE_${kind}_LINKER_FLAGS_RELEASE}")
endforeach()

# Static CRT: /MT, /MTd
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# C++23. CMake maps it to /std:c++latest on MSVC, which also enables
# C++26 work in progress, so pin /std:c++23preview (MSVC 19.43+; it stays an
# alias of /std:c++23 in later toolsets). /std:c++20 and later imply the
# /permissive- conformance mode.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.43)
	message(FATAL_ERROR "C++23 needs MSVC 19.43 (Visual Studio 2022 17.13) or newer, found ${CMAKE_CXX_COMPILER_VERSION}.")
endif()
set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "/std:c++23preview")
set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "/std:c++23preview")
# report the real standard in __cplusplus instead of 199711L
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(D3BFG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (/WX) in engine code" OFF)
# ---------------------------------------------------------------------------
# Common compile settings
# ---------------------------------------------------------------------------
set(D3BFG_NEO_DIR "${CMAKE_SOURCE_DIR}/neo")
set(D3BFG_ZLIB_DIR "${CMAKE_SOURCE_DIR}/third_party/zlib")
set(D3BFG_PCH "${D3BFG_NEO_DIR}/idlib/precompiled.h")
if(NOT EXISTS "${D3BFG_ZLIB_DIR}/zlib.h")
	message(FATAL_ERROR "third_party/zlib is empty: run git submodule update --init")
endif()

# Debug / Release(Retail) code generation, matching _Debug.props / _Release.props.
#   WITH_INLINES  - _WithInlines.props (Debug only: /Ob1 + _INLINEDEBUG)
#   WARNING_LEVEL - 3 or 4
#   WERROR        - honour D3BFG_WARNINGS_AS_ERRORS
function(d3bfg_configure_target tgt)
	cmake_parse_arguments(ARG "WITH_INLINES;WERROR" "WARNING_LEVEL" "" ${ARGN})
	if(NOT ARG_WARNING_LEVEL)
		set(ARG_WARNING_LEVEL 3)
	endif()

	target_compile_definitions(${tgt} PRIVATE
		$<$<CONFIG:Debug>:_DEBUG>
		$<$<NOT:$<CONFIG:Debug>>:NDEBUG>
	)
	target_compile_options(${tgt} PRIVATE
		/W${ARG_WARNING_LEVEL}
		/Zi
		$<$<CONFIG:Debug>:/Od /RTC1>
		$<$<NOT:$<CONFIG:Debug>>:/O2 /Ob2 /Oi /GF /GS- /Gy>
	)
	if(ARG_WITH_INLINES)
		target_compile_definitions(${tgt} PRIVATE $<$<CONFIG:Debug>:_INLINEDEBUG>)
		target_compile_options(${tgt} PRIVATE $<$<CONFIG:Debug>:/Ob1>)
	endif()
	if(ARG_WERROR AND D3BFG_WARNINGS_AS_ERRORS)
		target_compile_options(${tgt} PRIVATE /WX)
	endif()
	if(CMAKE_GENERATOR MATCHES "Visual Studio")
		target_compile_options(${tgt} PRIVATE /MP)
	endif()
endfunction()

# _Common.props
function(d3bfg_neo_common tgt)
	target_compile_definitions(${tgt} PRIVATE
		_CRT_SECURE_NO_DEPRECATE
		_CRT_NONSTDC_NO_DEPRECATE
	)
	target_include_directories(${tgt} PRIVATE "${D3BFG_NEO_DIR}" "${D3BFG_ZLIB_DIR}")
endfunction()

add_subdirectory(doomclassic)
enable_testing()
add_subdirectory(neo)
add_subdirectory(editor)
