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
set_source_files_properties(${EDITOR_BRIDGE_SOURCES} PROPERTIES COMPILE_OPTIONS /WX)
d3bfg_link_engine(Doom3Editor)
set_target_properties(Doom3Editor PROPERTIES FOLDER "editor")
