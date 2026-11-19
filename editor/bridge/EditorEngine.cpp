// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include <filesystem>
#include <string>

#include "framework/EngineHost.h"
#include "sys/sys_window.h"
#include "sys/win32/win_entry.h"

#include "../api/EditorEngineApi.h"
#include "EditorEngine.h"

namespace {

constexpr char		EDITOR_TITLE[] = "Doom 3 Editor";

// the engine as the UI sees it, and the UI as the engine sees it
class idEditorEngineLocal final : public idEditorEngine, public idEngineHost {
public:
	idEditorEngineLocal( HINSTANCE instance_, const char * cmdLine_ ) : instance( instance_ ), cmdLine( cmdLine_ ) {}

	// idEditorEngine
	[[nodiscard]] int	ApiVersion() const override { return EDITOR_API_VERSION; }
	void				Init( idEditorHost * host ) override;
	void				Frame() override;
	void				Shutdown() override;
	void				ResizeViewport( int width, int height ) override;
	void				FocusViewport() override;
	[[nodiscard]] bool	ViewportHasFocus() const override;
	void				ExecuteCommand( const char * text ) override;

	// idEngineHost
	void *				ParentWindow() override { return ui->ViewportParent(); }
	void				GetViewportSize( int & width, int & height ) override;
	void				PumpEvents() override;
	void				OnQuitRequested() override { ui->OnQuitRequested(); }
	void				OnFatalError( const char * text ) override { ui->OnFatalError( text ); }
	void				OnPrint( const char * text ) override { ui->OnPrint( text ); }

private:
	[[nodiscard]] HWND	Viewport() const { return started ? static_cast<HWND>( Sys_GetNativeWindow() ) : nullptr; }

	HINSTANCE			instance;
	std::string			cmdLine;
	idEditorHost *		ui = nullptr;
	bool				started = false;
	bool				inFrame = false;
};

void idEditorEngineLocal::Init( idEditorHost * host ) {
	ui = host;
	Sys_EngineInit( instance, cmdLine.c_str(), this );
	started = true;
}

void idEditorEngineLocal::Frame() {
	// a frame started from the UI's events inside a frame would run the engine inside itself
	if ( !started || inFrame ) {
		return;
	}
	inFrame = true;
	Sys_EngineFrame();
	inFrame = false;
}

void idEditorEngineLocal::Shutdown() {
	Sys_EngineShutdown();
}

void idEditorEngineLocal::ResizeViewport( int width, int height ) {
	if ( const HWND viewport = Viewport() ) {
		SetWindowPos( viewport, nullptr, 0, 0, Max( 1, width ), Max( 1, height ), SWP_NOZORDER | SWP_NOACTIVATE );
	}
}

void idEditorEngineLocal::FocusViewport() {
	if ( const HWND viewport = Viewport() ) {
		SetFocus( viewport );
	}
}

bool idEditorEngineLocal::ViewportHasFocus() const {
	const HWND viewport = Viewport();
	return viewport != nullptr && GetFocus() == viewport;
}

void idEditorEngineLocal::ExecuteCommand( const char * text ) {
	if ( !started || text == nullptr || text[0] == '\0' ) {
		return;
	}
	common->Printf( "]%s\n", text );
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "%s\n", text ) );
}

void idEditorEngineLocal::GetViewportSize( int & width, int & height ) {
	ui->GetViewportSize( width, height );
	width = Max( 1, width );
	height = Max( 1, height );
}

void idEditorEngineLocal::PumpEvents() {
	// between frames the UI's own loop dispatches; this is for the long work inside one
	if ( !started || inFrame ) {
		ui->PumpEvents();
	}
}

[[nodiscard]] std::filesystem::path ExeDirectory() {
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW( nullptr, path, MAX_PATH );
	return std::filesystem::path( path ).parent_path();
}

}	// namespace

int Editor_RunUi( HINSTANCE instance, const char * engineCmdLine ) {
	// the module's own folder holds the Qt DLLs it imports
	const std::filesystem::path modulePath = ExeDirectory() / EDITOR_UI_MODULE_PATH;
	const HMODULE module = LoadLibraryExW( modulePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH );
	const auto uiMain = ( module != nullptr ) ? reinterpret_cast<editorUiMain_t>( GetProcAddress( module, EDITOR_UI_MAIN_NAME ) ) : nullptr;
	if ( uiMain == nullptr ) {
		const std::wstring text = L"The editor's user interface could not be loaded:\n" + modulePath.wstring() +
			L"\n\nIt is built when Qt is installed (tools\\qt\\install-qt.ps1) and has to be copied with its folder."
			L"\n\"Doom3Editor.exe --bare\" runs the viewport alone.";
		MessageBoxW( nullptr, text.c_str(), L"Doom 3 Editor", MB_OK | MB_ICONERROR );
		return 1;
	}

	static idEditorEngineLocal engine( instance, engineCmdLine );
	const int result = uiMain( &engine, __argc, __argv );
	MessageBoxA( nullptr, "The editor's user interface could not start.", EDITOR_TITLE, MB_OK | MB_ICONERROR );
	return ( result != 0 ) ? result : 1;
}
