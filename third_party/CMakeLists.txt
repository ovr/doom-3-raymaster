# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.

# NVIDIA NRD, the denoiser of the ray traced passes: a static library whose DXIL ShaderMake compiles with our pinned
# DXC. MathLib and ShaderMake come from their submodules, never from the network (NRD's FetchContent is redirected).
# The block keeps NRD's own C++17 and gives back the per-config flags that the root CMakeLists.txt clears.
block()
	set(CMAKE_CXX_STANDARD 17)
	foreach(lang C CXX)
		set(CMAKE_${lang}_FLAGS_DEBUG "/Zi /Od /RTC1")
		set(CMAKE_${lang}_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG")
		set(CMAKE_${lang}_FLAGS_RETAIL "/O2 /Ob2 /DNDEBUG")
	endforeach()
	# NRD puts its library at CMAKE_RUNTIME_OUTPUT_DIRECTORY: keep it out of bin/ beside the exe
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

	set(FETCHCONTENT_SOURCE_DIR_MATHLIB "${CMAKE_CURRENT_SOURCE_DIR}/MathLib")
	set(FETCHCONTENT_SOURCE_DIR_SHADERMAKE "${CMAKE_CURRENT_SOURCE_DIR}/ShaderMake")

	set(SHADERMAKE_FIND_DXC OFF)
	set(SHADERMAKE_FIND_DXC_VK OFF)
	set(SHADERMAKE_FIND_FXC OFF)
	set(SHADERMAKE_FIND_SLANG OFF)
	set(SHADERMAKE_DXC_PATH "${D3BFG_D3D12_SDK_ROOT}/dxc/bin/x64/dxc.exe")

	set(NRD_STATIC_LIBRARY ON)
	set(NRD_EMBEDS_DXIL_SHADERS ON)
	set(NRD_EMBEDS_DXBC_SHADERS OFF)
	set(NRD_EMBEDS_SPIRV_SHADERS OFF)
	set(NRD_NORMAL_ENCODING 2)    # R10_G10_B10_A2_UNORM: octahedral, 2 bits of material ID
	set(NRD_ROUGHNESS_ENCODING 1) # LINEAR
	set(REBLUR_PERFORMANCE_MODE ${D3BFG_NRD_PERFORMANCE_MODE})
	set(NRD_SHADERS_PATH "${CMAKE_CURRENT_BINARY_DIR}/NRD/Shaders")

	add_subdirectory(NRD "${CMAKE_CURRENT_BINARY_DIR}/NRD" SYSTEM)
endblock()

foreach(_target NRD NRDShaders NRDIntegration ShaderMake ShaderMakeBlob MathLib)
	if(TARGET ${_target})
		set_target_properties(${_target} PROPERTIES FOLDER "third_party/NRD")
	endif()
endforeach()

# Our render programs include NRD.hlsli at run time, read through the file system from base/shaders (which refuses
# paths out of it), so the build copies it there with the NRDConfig.hlsli that NRD's configure wrote (gitignored).
foreach(_file NRD.hlsli NRDConfig.hlsli)
	configure_file("${CMAKE_CURRENT_SOURCE_DIR}/NRD/Shaders/${_file}"
		"${CMAKE_SOURCE_DIR}/base/shaders/include/nrd/${_file}" COPYONLY)
endforeach()
