// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

// The engine's entry point: starts it and runs the frame loop, never returns. Needs <windows.h> first;
// no engine header, so an executable's WinMain can call it without the precompiled header.
int Sys_GameMain( HINSTANCE hInstance, const char *cmdLine );
