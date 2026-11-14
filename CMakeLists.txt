cmake_minimum_required(VERSION 3.21)

project(Doom3BFG LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Platform checks: the code base is Win32 / MSVC / 32-bit only.
# ---------------------------------------------------------------------------
if(NOT MSVC)
	message(FATAL_ERROR "Doom 3 BFG can only be built with MSVC.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(FATAL_ERROR
		"Doom 3 BFG must be built as 32-bit (x86). "
		"Use the Visual Studio generator with '-A Win32' or an x86 Native Tools prompt.")
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

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(D3BFG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (/WX) in engine code" OFF)
# ---------------------------------------------------------------------------
# Common compile settings
# ---------------------------------------------------------------------------
set(D3BFG_NEO_DIR "${CMAKE_SOURCE_DIR}/neo")

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
		# /Oy- keeps frame pointers so profilers (and sys_profile) can walk call stacks
		$<$<NOT:$<CONFIG:Debug>>:/O2 /Ob2 /Oi /GF /GS- /Gy /Oy->
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
		_USE_32BIT_TIME_T
	)
	target_include_directories(${tgt} PRIVATE "${D3BFG_NEO_DIR}")
endfunction()

add_subdirectory(doomclassic)
enable_testing()
add_subdirectory(neo)
