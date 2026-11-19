// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

// The two ways Doom3Editor.exe runs the engine. Both end the process themselves and return only
// when they could not start. Needs <windows.h> first.

// the Qt interface from editor\Doom3EditorUI.dll
int		Editor_RunUi( HINSTANCE instance, const char * engineCmdLine );

// a plain Win32 frame around the viewport, with the game's own input
int		Editor_RunBare( HINSTANCE instance, const char * engineCmdLine );
