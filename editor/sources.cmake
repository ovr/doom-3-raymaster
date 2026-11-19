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
	bridge/EditorMain.cpp
	bridge/EditorView.cpp
	bridge/EditorView.h
	bridge/EditorWorld.cpp
	bridge/EditorWorld.h
)

# the Qt half: no engine header, no precompiled header, only api/ is shared with the bridge
set(EDITOR_UI_SOURCES
	api/EditorEngineApi.h
	ui/EngineClient.cpp
	ui/EngineClient.h
	ui/MainWindow.cpp
	ui/MainWindow.h
	ui/OutputLog.cpp
	ui/OutputLog.h
	ui/Outliner.cpp
	ui/Outliner.h
	ui/UiMain.cpp
	ui/ViewportHost.cpp
	ui/ViewportHost.h
)
