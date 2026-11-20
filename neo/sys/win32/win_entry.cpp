// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The game's WinMain, alone in its file: the engine objects are linked into other executables too.

#include <windows.h>

#include "win_entry.h"

int WINAPI WinMain( _In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR lpCmdLine, _In_ int ) {
	return Sys_GameMain( hInstance, lpCmdLine );
}
