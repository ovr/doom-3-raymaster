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
#include "EditorHistory.h"
#include "EditorTool.h"
#include "EditorView.h"
#include "EditorWorld.h"

namespace {

constexpr char		EDITOR_TITLE[] = "Doom 3 Editor";
constexpr float		PICK_DISTANCE = 16384.0f;
constexpr float		CREATE_DISTANCE = 192.0f;		// a new entity appears this far in front of the camera
constexpr float		CREATE_WALL_GAP = 24.0f;
constexpr float		DUPLICATE_OFFSET = 16.0f;
constexpr int		EDIT_FRAME_MSEC = 8;			// the edit view's frame time: 125 frames a second

// the engine as the UI sees it, and the UI as the engine sees it
class idEditorEngineLocal final : public idEditorEngine, public idEngineHost, public idEditorViewClient {
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
	void				SelectEntity( int index, bool add ) override;
	void				EnumSelection( editorSelectionVisitor_t visitor, void * context ) override;
	void				SetGizmo( int gizmo ) override;
	void				SetGridSize( float size ) override { tool.SetGridSize( size ); }
	void				EnumEntityKeys( int index, editorKeyVisitor_t visitor, void * context ) override;
	void				SetEntityKey( int index, const char * key, const char * value ) override;
	void				EnumEntityClasses( editorClassVisitor_t visitor, void * context ) override;
	void				CreateEntity( const char * classname ) override;
	void				DuplicateSelection() override;
	void				DeleteSelection() override;
	void				GetDocumentState( editorDocumentState_t & state ) const override;
	void				Undo() override { cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "editorUndo\n" ); }
	void				Redo() override { cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "editorRedo\n" ); }
	void				SaveMap() override { cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "editorSaveMap\n" ); }

	// the console commands behind the queued calls
	void				OpenMapNow( const char * name );
	void				CloseMapNow();
	void				UndoNow();
	void				RedoNow();
	void				SaveMapNow();
	// value nullptr removes the key
	void				SetKeyNow( const char * entityName, const char * key, const char * value );
	// A console command runs inside the frame, between its parts, where an edit is as safe as
	// between two frames.
	template<typename editFunction>
	void				RunEdit( editFunction && edit ) {
		const bool wasInFrame = inFrame;
		inFrame = false;
		edit();
		inFrame = wasInFrame;
	}
	// -1 and a print when the open map has no such entity
	[[nodiscard]] int	FindEntity( const char * entityName ) const;

private:
	[[nodiscard]] bool	IsEntity( int index ) const { return index > 0 && index < world.NumEntities() && !world.Entity( index ).removed; }
	// no edit while the engine runs a frame or a drag holds the entities
	[[nodiscard]] bool	CanEdit() const { return world.IsOpen() && !inFrame && !tool.Dragging(); }
	// applies the changes as one step of the history and tells the UI
	void				Edit( const char * label, const idList<editorChange_t> & changes );
	void				AfterEdit( bool listChanged );
	void				SelectOnly( const idList<int> & indices );

public:
	// idEditorViewClient
	void				ViewMouseDown( int x, int y, bool ctrl ) override;
	void				ViewMouseMove( int x, int y ) override;
	void				ViewMouseUp() override;

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
	int					lastFrameMsec = 0;
	idEditorWorld		world;
	idEditorView		view;
	idEditorTool		tool;
	idEditorHistory		history;
};

// for the console commands; nullptr in the bare shell
idEditorEngineLocal *	editorEngine = nullptr;

void idEditorEngineLocal::Init( idEditorHost * host ) {
	ui = host;
	editorEngine = this;
	Sys_EngineInit( instance, cmdLine.c_str(), this );
	started = true;
	// an entity of any class can be put into any map
	fileSystem->MountAllMapResources();
	view.Attach( Viewport() );
	view.SetClient( this );
}

void idEditorEngineLocal::Frame() {
	// a frame started from the UI's events inside a frame would run the engine inside itself
	if ( !started || inFrame ) {
		return;
	}
	// Nothing paces the edit view (no game tics), and an editor has no use for a thousand frames a second.
	if ( world.IsOpen() ) {
		const int sinceLast = Sys_Milliseconds() - lastFrameMsec;
		if ( sinceLast >= 0 && sinceLast < EDIT_FRAME_MSEC ) {
			Sys_Sleep( EDIT_FRAME_MSEC - sinceLast );
		}
	}
	lastFrameMsec = Sys_Milliseconds();

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
	tool.Reset();
	history.Clear();
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
	tool.Reset();
	history.Clear();
	world.Close();
	view.SetEditMode( false );
	ui->OnMapChanged();
}

void idEditorEngineLocal::UndoNow() {
	if ( world.IsOpen() && history.CanUndo() && !tool.Dragging() ) {
		AfterEdit( history.Undo( world ) );
	}
}

void idEditorEngineLocal::RedoNow() {
	if ( world.IsOpen() && history.CanRedo() && !tool.Dragging() ) {
		AfterEdit( history.Redo( world ) );
	}
}

void idEditorEngineLocal::SaveMapNow() {
	if ( !world.IsOpen() ) {
		common->Printf( "No map is open in the editor.\n" );
		return;
	}
	if ( world.Save() ) {
		history.MarkSaved();
		ui->OnDocumentChanged();
	} else {
		common->Warning( "Editor: %s could not be saved", world.Name() );
	}
}

int idEditorEngineLocal::FindEntity( const char * entityName ) const {
	const int index = world.IsOpen() ? world.FindEntity( entityName ) : -1;
	if ( index == -1 ) {
		common->Printf( "The open map has no entity named '%s'.\n", entityName );
	}
	return index;
}

void idEditorEngineLocal::SetKeyNow( const char * entityName, const char * key, const char * value ) {
	const int index = FindEntity( entityName );
	if ( index != -1 ) {
		RunEdit( [&] { SetEntityKey( index, key, value ); } );
	}
}

void idEditorEngineLocal::Edit( const char * label, const idList<editorChange_t> & changes ) {
	if ( changes.Num() == 0 ) {
		return;
	}
	for ( int i = 0; i < changes.Num(); i++ ) {
		world.SetMapArgs( changes[i].index, changes[i].after );
	}
	history.Add( label, changes );
	AfterEdit( idEditorHistory::ChangesList( changes ) );
}

void idEditorEngineLocal::AfterEdit( bool listChanged ) {
	const bool selectionChanged = tool.DropRemoved( world );
	if ( listChanged ) {
		ui->OnEntitiesChanged();
	}
	if ( selectionChanged || listChanged ) {
		ui->OnSelectionChanged();
	}
	ui->OnDocumentChanged();
}

void idEditorEngineLocal::SelectOnly( const idList<int> & indices ) {
	tool.ClearSelection();
	for ( int i = 0; i < indices.Num(); i++ ) {
		tool.Select( indices[i], true );
	}
	ui->OnSelectionChanged();
}

void idEditorEngineLocal::SetEntityKey( int index, const char * key, const char * value ) {
	if ( !CanEdit() || !IsEntity( index ) || key == nullptr || key[0] == '\0' ) {
		return;
	}
	idList<editorChange_t> changes;
	editorChange_t & change = changes.Alloc();
	change.index = index;
	change.before = world.MapArgs( index );
	change.after = change.before;
	if ( value != nullptr ) {
		change.after.Set( key, value );
	} else {
		change.after.Delete( key );
	}
	// an entity without a class is a removed one
	if ( change.after.GetString( "classname" )[0] == '\0' ) {
		common->Printf( "An entity needs its classname.\n" );
		return;
	}
	const idKeyValue * const was = change.before.FindKey( key );
	const idKeyValue * const is = change.after.FindKey( key );
	if ( ( was == nullptr ) == ( is == nullptr ) && ( was == nullptr || was->GetValue() == is->GetValue() ) ) {
		return;
	}
	Edit( va( "Set %s", key ), changes );
}

// The entityDef documents a key as "editor_<kind> <key>" "<description>".
void idEditorEngineLocal::EnumEntityKeys( int index, editorKeyVisitor_t visitor, void * context ) {
	if ( !IsEntity( index ) ) {
		return;
	}
	const idDict & mapArgs = world.MapArgs( index );
	const idDict & allArgs = world.Entity( index ).args;
	const idDict * const def = gameEdit->FindEntityDefDict( mapArgs.GetString( "classname" ), false );

	const auto documented = [def]( const char * key, const char * & description ) -> int {
		description = "";
		int type = ( idStr::Icmp( key, "_color" ) == 0 ) ? EDITOR_KEY_COLOR : EDITOR_KEY_TEXT;
		if ( def == nullptr ) {
			return type;
		}
		for ( const idKeyValue * kv = def->MatchPrefix( "editor_" ); kv != nullptr; kv = def->MatchPrefix( "editor_", kv ) ) {
			const int space = kv->GetKey().Find( ' ' );
			if ( space != -1 && idStr::Icmp( kv->GetKey().c_str() + space + 1, key ) == 0 ) {
				description = kv->GetValue().c_str();
				if ( kv->GetKey().Icmpn( "editor_bool ", 12 ) == 0 ) {
					type = EDITOR_KEY_BOOL;
				} else if ( kv->GetKey().Icmpn( "editor_color ", 13 ) == 0 ) {
					type = EDITOR_KEY_COLOR;
				}
				break;
			}
		}
		return type;
	};

	for ( int i = 0; i < mapArgs.GetNumKeyVals(); i++ ) {
		const idKeyValue * const kv = mapArgs.GetKeyVal( i );
		editorKeyInfo_t info = { kv->GetKey().c_str(), kv->GetValue().c_str(), "", EDITOR_KEY_TEXT, true };
		info.type = documented( info.key, info.description );
		visitor( context, info );
	}
	if ( def == nullptr ) {
		return;
	}
	for ( const idKeyValue * kv = def->MatchPrefix( "editor_" ); kv != nullptr; kv = def->MatchPrefix( "editor_", kv ) ) {
		const int space = kv->GetKey().Find( ' ' );
		if ( space == -1 ) {
			continue;
		}
		const char * const key = kv->GetKey().c_str() + space + 1;
		if ( key[0] == '\0' || mapArgs.FindKey( key ) != nullptr ) {
			continue;
		}
		editorKeyInfo_t info = { key, allArgs.GetString( key ), "", EDITOR_KEY_TEXT, false };
		info.type = documented( key, info.description );
		visitor( context, info );
	}
}

// Only the names: parsing an entityDef caches its media.
void idEditorEngineLocal::EnumEntityClasses( editorClassVisitor_t visitor, void * context ) {
	if ( !started ) {
		return;
	}
	const int num = declManager->GetNumDecls( DECL_ENTITYDEF );
	for ( int i = 0; i < num; i++ ) {
		visitor( context, declManager->DeclByIndex( DECL_ENTITYDEF, i, false )->GetName() );
	}
}

void idEditorEngineLocal::CreateEntity( const char * classname ) {
	if ( !CanEdit() ) {
		return;
	}
	if ( gameEdit->FindEntityDefDict( classname, false ) == nullptr ) {
		common->Printf( "There is no entityDef '%s'.\n", classname );
		return;
	}
	// in front of the camera, before the wall it looks at
	const idVec3 forward = view.Axis()[0];
	float distance = CREATE_DISTANCE;
	modelTrace_t trace = {};
	if ( world.RenderWorld()->Trace( trace, view.Origin(), view.Origin() + forward * CREATE_DISTANCE, 0.0f, false ) ) {
		distance = Max( CREATE_WALL_GAP, trace.fraction * CREATE_DISTANCE - CREATE_WALL_GAP );
	}
	idVec3 origin = view.Origin() + forward * distance;
	if ( tool.GridSize() > 0.0f ) {
		for ( int axis = 0; axis < 3; axis++ ) {
			origin[axis] = idMath::Rint( origin[axis] / tool.GridSize() ) * tool.GridSize();
		}
	}

	idList<editorChange_t> changes;
	editorChange_t & change = changes.Alloc();
	change.index = world.AddEntity();
	change.after.Set( "classname", classname );
	change.after.Set( "name", world.UniqueName( classname ) );
	change.after.SetVector( "origin", origin );
	Edit( va( "Create %s", classname ), changes );

	idList<int> created;
	created.Append( change.index );
	SelectOnly( created );
}

void idEditorEngineLocal::DuplicateSelection() {
	if ( !CanEdit() ) {
		return;
	}
	const float step = ( tool.GridSize() > 0.0f ) ? tool.GridSize() * 2.0f : DUPLICATE_OFFSET;
	idList<editorChange_t> changes;
	idList<int> copies;
	for ( int i = 0; i < tool.NumSelected(); i++ ) {
		const int source = tool.Selected( i );
		// a brush entity's geometry is compiled into the map
		if ( !IsEntity( source ) || world.HasPrimitives( source ) ) {
			continue;
		}
		editorChange_t & change = changes.Alloc();
		change.index = world.AddEntity();
		change.after = world.MapArgs( source );
		change.after.Set( "name", world.UniqueName( change.after.GetString( "classname" ) ) );
		if ( change.after.FindKey( "origin" ) != nullptr ) {
			change.after.SetVector( "origin", change.after.GetVector( "origin" ) + idVec3( step, step, 0.0f ) );
		}
		// shown now, so the next copy gets another name
		world.SetMapArgs( change.index, change.after );
		copies.Append( change.index );
	}
	if ( changes.Num() > 0 ) {
		Edit( ( changes.Num() == 1 ) ? "Duplicate" : va( "Duplicate %d entities", changes.Num() ), changes );
		SelectOnly( copies );
	}
}

void idEditorEngineLocal::DeleteSelection() {
	if ( !CanEdit() ) {
		return;
	}
	idList<editorChange_t> changes;
	for ( int i = 0; i < tool.NumSelected(); i++ ) {
		if ( IsEntity( tool.Selected( i ) ) ) {
			editorChange_t & change = changes.Alloc();
			change.index = tool.Selected( i );
			change.before = world.MapArgs( change.index );
		}
	}
	Edit( ( changes.Num() == 1 ) ? "Delete" : va( "Delete %d entities", changes.Num() ), changes );
}

void idEditorEngineLocal::SelectEntity( int index, bool add ) {
	if ( ( index > 0 && !IsEntity( index ) ) || tool.Dragging() ) {
		return;
	}
	if ( tool.Select( index, add ) ) {
		ui->OnSelectionChanged();
	}
}

void idEditorEngineLocal::EnumSelection( editorSelectionVisitor_t visitor, void * context ) {
	for ( int i = 0; i < tool.NumSelected(); i++ ) {
		visitor( context, tool.Selected( i ) );
	}
}

void idEditorEngineLocal::SetGizmo( int gizmo ) {
	if ( !tool.Dragging() ) {
		tool.SetMode( ( gizmo == EDITOR_GIZMO_ROTATE ) ? idEditorTool::mode_t::ROTATE : idEditorTool::mode_t::MOVE );
	}
}

void idEditorEngineLocal::GetDocumentState( editorDocumentState_t & state ) const {
	state.dirty = history.IsDirty();
	state.canUndo = history.CanUndo();
	state.canRedo = history.CanRedo();
	state.undoLabel = history.UndoLabel();
	state.redoLabel = history.RedoLabel();
}

void idEditorEngineLocal::ViewMouseDown( int x, int y, bool ctrl ) {
	if ( !world.IsOpen() ) {
		return;
	}
	const idVec3 dir = view.RayThroughPixel( x, y );
	if ( tool.BeginDrag( world, view.Origin(), dir, view.Origin() ) ) {
		return;
	}
	if ( tool.Select( world.Pick( view.Origin(), dir, PICK_DISTANCE ), ctrl ) ) {
		ui->OnSelectionChanged();
	}
}

void idEditorEngineLocal::ViewMouseMove( int x, int y ) {
	if ( tool.Dragging() ) {
		tool.Drag( world, view.Origin(), view.RayThroughPixel( x, y ) );
	}
}

void idEditorEngineLocal::ViewMouseUp() {
	if ( tool.EndDrag( world, history ) ) {
		ui->OnDocumentChanged();
	}
}

void idEditorEngineLocal::EnumEntities( editorEntityVisitor_t visitor, void * context ) {
	for ( int i = 0; i < world.NumEntities(); i++ ) {
		const editorMapEntity_t & entity = world.Entity( i );
		if ( entity.removed ) {
			continue;
		}
		const editorEntityInfo_t info = { i, entity.args.GetString( "name" ), entity.args.GetString( "classname" ), entity.isLight, entity.modelHandle != -1 };
		visitor( context, info );
	}
}

void idEditorEngineLocal::LookAtEntity( int index ) {
	if ( !IsEntity( index ) ) {
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
	// The lines of the last frame: the game clears them every tic, nothing does here, and a full
	// line buffer silently drops the new ones.
	const int now = Sys_Milliseconds();
	world.RenderWorld()->DebugClearLines( now );
	tool.Draw( world, view.Origin() );
	float fovX, fovY;
	view.GetFov( fovX, fovY );
	world.Draw( view.Origin(), view.Axis(), fovX, fovY, now );
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

CONSOLE_COMMAND_SHIP( editorUndo, "undoes the last edit of the map open in the editor", nullptr ) {
	if ( NeedEditor() ) {
		editorEngine->UndoNow();
	}
}

CONSOLE_COMMAND_SHIP( editorRedo, "redoes the edit undone last", nullptr ) {
	if ( NeedEditor() ) {
		editorEngine->RedoNow();
	}
}

CONSOLE_COMMAND_SHIP( editorSetKey, "sets a key of an entity of the open map, or removes it: editorSetKey <entity> <key> [value]", nullptr ) {
	if ( args.Argc() < 3 || args.Argc() > 4 ) {
		common->Printf( "usage: editorSetKey <entity name> <key> [value]\n" );
		return;
	}
	if ( NeedEditor() ) {
		editorEngine->SetKeyNow( args.Argv( 1 ), args.Argv( 2 ), ( args.Argc() == 4 ) ? args.Argv( 3 ) : nullptr );
	}
}

CONSOLE_COMMAND_SHIP( editorSelect, "selects an entity of the open map by name and puts the camera in front of it", nullptr ) {
	if ( args.Argc() != 2 ) {
		common->Printf( "usage: editorSelect <entity name>\n" );
		return;
	}
	if ( NeedEditor() ) {
		const int index = editorEngine->FindEntity( args.Argv( 1 ) );
		if ( index != -1 ) {
			editorEngine->SelectEntity( index, false );
			editorEngine->LookAtEntity( index );
		}
	}
}

CONSOLE_COMMAND_SHIP( editorCreate, "makes an entity of a class in front of the editor's camera", nullptr ) {
	if ( args.Argc() != 2 ) {
		common->Printf( "usage: editorCreate <entityDef>\n" );
		return;
	}
	if ( NeedEditor() ) {
		editorEngine->RunEdit( [&args] { editorEngine->CreateEntity( args.Argv( 1 ) ); } );
	}
}

CONSOLE_COMMAND_SHIP( editorDuplicate, "duplicates the entities selected in the editor", nullptr ) {
	if ( NeedEditor() ) {
		editorEngine->RunEdit( [] { editorEngine->DuplicateSelection(); } );
	}
}

CONSOLE_COMMAND_SHIP( editorDelete, "removes the entities selected in the editor", nullptr ) {
	if ( NeedEditor() ) {
		editorEngine->RunEdit( [] { editorEngine->DeleteSelection(); } );
	}
}

CONSOLE_COMMAND_SHIP( editorSaveMap, "saves the map open in the editor into fs_savepath", nullptr ) {
	if ( NeedEditor() ) {
		editorEngine->SaveMapNow();
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
