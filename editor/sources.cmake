# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.

# the engine-side half of the editor, compiled with the engine's precompiled header
set(EDITOR_BRIDGE_SOURCES
	api/EditorEngineApi.h
	bridge/EditorBare.cpp
	bridge/EditorCheck.cpp
	bridge/EditorEngine.cpp
	bridge/EditorEngine.h
	bridge/EditorHistory.cpp
	bridge/EditorHistory.h
	bridge/EditorMain.cpp
	bridge/EditorPreview.cpp
	bridge/EditorPreview.h
	bridge/EditorTool.cpp
	bridge/EditorTool.h
	bridge/EditorView.cpp
	bridge/EditorView.h
	bridge/EditorWorld.cpp
	bridge/EditorWorld.h
	mtr/MtrText.h
)

# the map compilers (dmap), ported from the Doom 3 GPL release; the engine's precompiled header
set(MAP_COMPILERS_SOURCES
	compilers/dmap/dmap.cpp
	compilers/dmap/dmap.h
	compilers/dmap/DmapCheck.cpp
	compilers/dmap/DmapPatch.cpp
	compilers/dmap/DmapPatch.h
	compilers/dmap/DmapVert.h
	compilers/dmap/facebsp.cpp
	compilers/dmap/leakfile.cpp
	compilers/dmap/map.cpp
	compilers/dmap/optimize.cpp
	compilers/dmap/output.cpp
	compilers/dmap/portals.cpp
	compilers/dmap/tritjunction.cpp
	compilers/dmap/tritools.cpp
	compilers/dmap/ubrush.cpp
	compilers/dmap/usurface.cpp
)

# the Qt half: no engine header, no precompiled header, only api/ is shared with the bridge
set(EDITOR_UI_SOURCES
	api/EditorEngineApi.h
	ui/ContentBrowser.cpp
	ui/ContentBrowser.h
	ui/EngineClient.cpp
	ui/EngineClient.h
	ui/Inspector.cpp
	ui/Inspector.h
	mtr/MtrText.h
	ui/MainWindow.cpp
	ui/MainWindow.h
	ui/MaterialEditor.cpp
	ui/MaterialEditor.h
	ui/OutputLog.cpp
	ui/OutputLog.h
	ui/Outliner.cpp
	ui/Outliner.h
	ui/Palette.cpp
	ui/Palette.h
	ui/ThumbnailCache.cpp
	ui/ThumbnailCache.h
	ui/UiMain.cpp
	ui/ViewportHost.cpp
	ui/ViewportHost.h
)
