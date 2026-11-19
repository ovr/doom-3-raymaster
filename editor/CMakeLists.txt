# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.

# The editor: Doom3Editor.exe is the engine (the same objects as the game) plus the bridge, the
# engine-side half of the editor. The Qt user interface is a separate DLL, so that no Qt code
# shares a module with the engine's operator new and its string macros.

include(sources.cmake)

# ---------------------------------------------------------------------------
# Doom3Editor executable
# ---------------------------------------------------------------------------
add_executable(Doom3Editor WIN32
	${EDITOR_BRIDGE_SOURCES}
	bridge/doom3editor.manifest
)
d3bfg_neo_common(Doom3Editor)
d3bfg_configure_target(Doom3Editor WARNING_LEVEL 4 WITH_INLINES WERROR)
target_compile_definitions(Doom3Editor PRIVATE
	__DOOM__ _MBCS
	$<$<CONFIG:Retail>:ID_RETAIL>
)
target_precompile_headers(Doom3Editor PRIVATE "${D3BFG_PCH}")
# new code without warnings: /WX in every config, per file so that the PCH of the old headers stays out
set(EDITOR_BRIDGE_CPP ${EDITOR_BRIDGE_SOURCES})
list(FILTER EDITOR_BRIDGE_CPP INCLUDE REGEX "\.cpp$")
set_source_files_properties(${EDITOR_BRIDGE_CPP} PROPERTIES COMPILE_OPTIONS /WX)
d3bfg_link_engine(Doom3Editor)
set_target_properties(Doom3Editor PROPERTIES FOLDER "editor")

# ---------------------------------------------------------------------------
# Doom3EditorUI.dll: the Qt user interface, in bin/<Config>/editor with the Qt DLLs
# ---------------------------------------------------------------------------
# tools/qt/install-qt.ps1 puts a kit into external/qt-x64; D3BFG_QT_ROOT points at another one
set(D3BFG_QT_ROOT "" CACHE PATH "Qt 6 kit for the editor UI (the folder with bin and lib), default: external/qt-x64/<version>/msvc2022_64")
set(EDITOR_QT_ROOT "${D3BFG_QT_ROOT}")
if(NOT EDITOR_QT_ROOT)
	file(GLOB EDITOR_QT_KITS "${CMAKE_SOURCE_DIR}/external/qt-x64/*/msvc2022_64")
	if(EDITOR_QT_KITS)
		list(SORT EDITOR_QT_KITS COMPARE NATURAL ORDER DESCENDING)
		list(GET EDITOR_QT_KITS 0 EDITOR_QT_ROOT)
	endif()
endif()
if(EDITOR_QT_ROOT)
	# Retail is Release for the imported Qt targets
	set(CMAKE_MAP_IMPORTED_CONFIG_RETAIL Release)
	find_package(Qt6 6.5 QUIET COMPONENTS Widgets PATHS "${EDITOR_QT_ROOT}" NO_DEFAULT_PATH)
endif()

if(NOT Qt6_FOUND)
	message(STATUS "Editor UI: Qt 6 not found, Doom3EditorUI is not built (run tools/qt/install-qt.ps1 or set D3BFG_QT_ROOT); Doom3Editor.exe --bare still works")
	return()
endif()
message(STATUS "Editor UI: Qt ${Qt6_VERSION} in ${EDITOR_QT_ROOT}")

add_library(Doom3EditorUI SHARED ${EDITOR_UI_SOURCES})
d3bfg_configure_target(Doom3EditorUI WARNING_LEVEL 4)
target_compile_options(Doom3EditorUI PRIVATE /WX)
target_link_libraries(Doom3EditorUI PRIVATE Qt6::Widgets)
target_link_options(Doom3EditorUI PRIVATE /DEBUG)
set_target_properties(Doom3EditorUI PROPERTIES
	AUTOMOC ON
	# Qt is built against the CRT DLL; the engine's static CRT stays in the exe
	MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/editor"
	FOLDER "editor"
)
# the exe loads the DLL at run time: build them together
add_dependencies(Doom3Editor Doom3EditorUI)

# the Qt DLLs and plugins next to the module; a stamp keeps it to once per kit and config
get_target_property(EDITOR_QMAKE Qt6::qmake IMPORTED_LOCATION)
get_filename_component(EDITOR_QT_BIN "${EDITOR_QMAKE}" DIRECTORY)
add_custom_command(TARGET Doom3EditorUI POST_BUILD
	COMMAND "${EDITOR_QT_BIN}/windeployqt.exe"
		--no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime
		--verbose 0 "$<TARGET_FILE:Doom3EditorUI>"
	VERBATIM
)
