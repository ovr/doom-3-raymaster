// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

// The engine's entry points. Needs <windows.h> first; no engine header, so an executable's
// WinMain can call them without the precompiled header.

class idEngineHost;

// the game: starts the engine and runs the frame loop, never returns
int		Sys_GameMain( HINSTANCE hInstance, const char *cmdLine );

// A host (framework/EngineHost.h) drives the same steps itself; host is nullptr for the game.
void	Sys_EngineInit( HINSTANCE hInstance, const char *cmdLine, idEngineHost *host );
void	Sys_EngineFrame();
// shuts the engine down and ends the process
void	Sys_EngineShutdown();
