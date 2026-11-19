// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

/*
	The boundary between the two halves of the editor, which live in one process:

	  Doom3Editor.exe (engine + bridge, static CRT)        editor\Doom3EditorUI.dll (Qt, CRT DLL)
	  ┌──────────────────────────────┐   idEditorEngine   ┌──────────────────────────────┐
	  │ WinMain ── LoadLibrary ──────┼───────────────────►│ EditorUiMain: QApplication,  │
	  │ idEditorEngineLocal          │◄───────────────────┼─ main window, frame timer    │
	  │ engine window (child HWND)   │   idEditorHost     │ viewport widget (parent HWND)│
	  └──────────────────────────────┘                    └──────────────────────────────┘

	The two modules have different heaps and C runtimes. So nothing here passes ownership: only
	plain data, const char * that is valid during the call, and interfaces nobody deletes through.
	The header includes nothing, neither the engine's headers nor Qt's.

	Everything runs on the main thread. The UI owns the message loop and calls Frame() from a timer;
	the engine may call the host back from inside Init() and Frame().
*/

constexpr int EDITOR_API_VERSION = 1;

// implemented by the UI
class idEditorHost {
public:
	// the HWND the engine window becomes a child of, and its size in device pixels
	virtual void *		ViewportParent() = 0;
	virtual void		GetViewportSize( int & width, int & height ) = 0;

	// Called inside long engine work (level loads) so that the windows stay alive.
	// The UI must not call Frame() or Shutdown() from here.
	virtual void		PumpEvents() = 0;

	// the engine's quit command: the UI closes and calls Shutdown()
	virtual void		OnQuitRequested() = 0;

	// a fatal engine error; the process ends when this returns
	virtual void		OnFatalError( const char * text ) = 0;

	// a console print, without colour codes
	virtual void		OnPrint( const char * text ) = 0;

protected:
						~idEditorHost() = default;
};

// implemented by the bridge
class idEditorEngine {
public:
	[[nodiscard]] virtual int	ApiVersion() const = 0;

	// starts the engine with its window inside host->ViewportParent(); a failure is a fatal error
	virtual void		Init( idEditorHost * host ) = 0;
	virtual void		Frame() = 0;
	// shuts the engine down and ends the process: it does not return
	virtual void		Shutdown() = 0;

	// the viewport follows its parent's size, in device pixels
	virtual void		ResizeViewport( int width, int height ) = 0;
	// The engine takes keys and the mouse while its window has the keyboard focus.
	// A click into the viewport gives it, Shift+Esc gives it back to the parent.
	virtual void		FocusViewport() = 0;
	[[nodiscard]] virtual bool	ViewportHasFocus() const = 0;

	// queues a console command for the next frame; safe from PumpEvents()
	virtual void		ExecuteCommand( const char * text ) = 0;

protected:
						~idEditorEngine() = default;
};

// the UI module's only export; returns when the UI could not start
using editorUiMain_t = int ( * )( idEditorEngine * engine, int argc, char ** argv );
constexpr char EDITOR_UI_MAIN_NAME[] = "EditorUiMain";
constexpr char EDITOR_UI_MODULE_PATH[] = "editor\\Doom3EditorUI.dll";
