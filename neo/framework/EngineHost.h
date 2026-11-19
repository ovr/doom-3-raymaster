// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

/*
	The seam for an executable that embeds the engine in its own window (the editor).

	engineHost is nullptr in the game, where every branch on it is dead. A host owns the top-level
	window and the message loop: the engine window is a child of the host's, the engine never
	pumps messages itself, and it shows no intro, legal splash or main menu.

	  host WinMain ── Sys_EngineInit( host ) ── loop { Sys_EngineFrame() } ── Sys_EngineShutdown()
	                        │                           │
	                  ParentWindow()              PumpEvents()   OnQuitRequested()   OnFatalError()
	                  GetViewportSize()           OnPrint()
*/
class idEngineHost {
public:
	virtual				~idEngineHost() = default;

	// the HWND the engine window becomes a child of, and the size in pixels the child starts with
	virtual void *		ParentWindow() = 0;
	virtual void		GetViewportSize( int & width, int & height ) = 0;

	// Replaces the engine's message pump: once a frame and inside long work (level loads).
	// The host dispatches what it can without calling back into the engine.
	virtual void		PumpEvents() = 0;

	// the quit command, or a quit from a menu: the host closes and calls Sys_EngineShutdown
	virtual void		OnQuitRequested() = 0;

	// a fatal engine error, the engine state can still be read; the process ends when this returns
	virtual void		OnFatalError( const char * text ) = 0;

	// every console print, colour codes removed, on the main thread
	virtual void		OnPrint( const char * text ) = 0;
};

extern idEngineHost *	engineHost;

// A hosted engine window is active (keys, mouse grab, sound) while it has the keyboard focus: the
// host gives it with SetFocus, a click takes it, and Shift+Esc hands it back to the parent window.
