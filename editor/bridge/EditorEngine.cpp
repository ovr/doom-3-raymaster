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
#include "EditorView.h"
#include "EditorWorld.h"

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
	void				EnumMaps( editorMapVisitor_t visitor, void * context ) override;
	void				OpenMap( const char * name ) override;
	void				CloseMap() override;
	[[nodiscard]] const char *	CurrentMap() const override { return world.Name(); }
	void				EnumEntities( editorEntityVisitor_t visitor, void * context ) override;
	void				LookAtEntity( int index ) override;

	// the console commands behind OpenMap() and CloseMap()
	void				OpenMapNow( const char * name );
	void				CloseMapNow();

	// idEngineHost
	void *				ParentWindow() override { return ui->ViewportParent(); }
	void				GetViewportSize( int & width, int & height ) override;
	void				PumpEvents() override;
	void				OnQuitRequested() override { ui->OnQuitRequested(); }
	void				OnFatalError( const char * text ) override { ui->OnFatalError( text ); }
	void				OnPrint( const char * text ) override { ui->OnPrint( text ); }
	bool				DrawScene() override;

private:
	[[nodiscard]] HWND	Viewport() const { return started ? static_cast<HWND>( Sys_GetNativeWindow() ) : nullptr; }

	HINSTANCE			instance;
	std::string			cmdLine;
	idEditorHost *		ui = nullptr;
	bool				started = false;
	bool				inFrame = false;
	idEditorWorld		world;
	idEditorView		view;
};

// for the console commands; nullptr in the bare shell
idEditorEngineLocal *	editorEngine = nullptr;

void idEditorEngineLocal::Init( idEditorHost * host ) {
	ui = host;
	editorEngine = this;
	Sys_EngineInit( instance, cmdLine.c_str(), this );
	started = true;
	view.Attach( Viewport() );
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

void idEditorEngineLocal::EnumMaps( editorMapVisitor_t visitor, void * context ) {
	const int num = declManager->GetNumDecls( DECL_MAPDEF );
	for ( int i = 0; i < num; i++ ) {
		const auto * const mapDef = static_cast<const idDeclEntityDef *>( declManager->DeclByIndex( DECL_MAPDEF, i ) );
		if ( mapDef == nullptr ) {
			continue;
		}
		const editorMapInfo_t info = { mapDef->GetName(), idLocalization::GetString( mapDef->dict.GetString( "name", mapDef->GetName() ) ) };
		visitor( context, info );
	}
}

void idEditorEngineLocal::OpenMap( const char * name ) {
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "editorOpenMap \"%s\"\n", name ) );
}

void idEditorEngineLocal::CloseMap() {
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "editorCloseMap\n" );
}

void idEditorEngineLocal::OpenMapNow( const char * name ) {
	if ( world.Open( name ) ) {
		idVec3 origin;
		idAngles angles;
		world.GetStartView( origin, angles );
		view.SetView( origin, angles );
	}
	view.SetEditMode( world.IsOpen() );
	ui->OnMapChanged();
}

void idEditorEngineLocal::CloseMapNow() {
	world.Close();
	view.SetEditMode( false );
	ui->OnMapChanged();
}

void idEditorEngineLocal::EnumEntities( editorEntityVisitor_t visitor, void * context ) {
	for ( int i = 0; i < world.NumEntities(); i++ ) {
		const editorMapEntity_t & entity = world.Entity( i );
		const editorEntityInfo_t info = { i, entity.args.GetString( "name" ), entity.args.GetString( "classname" ), entity.isLight, entity.modelHandle != -1 };
		visitor( context, info );
	}
}

void idEditorEngineLocal::LookAtEntity( int index ) {
	if ( index <= 0 || index >= world.NumEntities() ) {
		return;
	}
	// from where the camera is now, at a distance that shows the whole entity
	const editorMapEntity_t & entity = world.Entity( index );
	idVec3 toEntity = entity.origin - view.Origin();
	if ( toEntity.Normalize() < 1.0f ) {
		toEntity.Set( 1.0f, 0.0f, 0.0f );
	}
	const float distance = Max( 96.0f, entity.bounds.GetRadius() * 3.0f );
	view.SetView( entity.origin - toEntity * distance, toEntity.ToAngles() );
}

bool idEditorEngineLocal::DrawScene() {
	if ( !world.IsOpen() ) {
		return false;
	}
	view.Frame();
	world.Draw( view.Origin(), view.Axis(), Sys_Milliseconds() );
	return true;
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

[[nodiscard]] bool NeedEditor() {
	if ( editorEngine == nullptr ) {
		common->Printf( "This command needs the editor's interface: start Doom3Editor.exe without --bare.\n" );
		return false;
	}
	return true;
}

}	// namespace

CONSOLE_COMMAND_SHIP( editorOpenMap, "opens a map in the editor: editorOpenMap game/mc_underground", idCmdSystem::ArgCompletion_MapName ) {
	if ( args.Argc() != 2 ) {
		common->Printf( "usage: editorOpenMap <map>\n" );
		return;
	}
	if ( NeedEditor() ) {
		editorEngine->OpenMapNow( args.Argv( 1 ) );
	}
}

CONSOLE_COMMAND_SHIP( editorCloseMap, "closes the map open in the editor", nullptr ) {
	if ( NeedEditor() ) {
		editorEngine->CloseMapNow();
	}
}

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
