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
#include "renderer/Image.h"

#include "EditorEngine.h"
#include "EditorHistory.h"
#include "EditorPreview.h"
#include "../mtr/MtrText.h"
#include "EditorTool.h"
#include "EditorView.h"
#include "EditorWorld.h"

namespace {

constexpr char		EDITOR_TITLE[] = "Doom 3 Editor";
constexpr float		PICK_DISTANCE = 16384.0f;
constexpr float		EMPTY_VIEW_GREY = 0.11f;
constexpr char		GENERATED_MODELS[] = "generated/rendermodels/";
constexpr int		PREVIEW_ERROR_PAUSE_MSEC = 250;
constexpr int		PREVIEW_TEST_ID = -1;
constexpr int		PREVIEW_SWEEP_ID = -2;			// the pictures of editorPreviewSweep, thrown away			// the picture of editorPreviewTest
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
	void				CreateEntity( const char * classname, const char * model ) override;
	void				EnumAssets( int kind, editorAssetVisitor_t visitor, void * context ) override;
	[[nodiscard]] bool	GetMaterial( const char * name, editorMaterialVisitor_t visitor, void * context ) override;
	[[nodiscard]] bool	ApplyMaterialText( const char * name, const char * text ) override;
	[[nodiscard]] bool	SaveMaterial( const char * name ) override;
	void				EditMaterial( const char * name ) { ui->OnEditMaterial( name ); }
	void				PlaySound( const char * name ) override;
	void				DuplicateSelection() override;
	void				DeleteSelection() override;
	void				GetDocumentState( editorDocumentState_t & state ) const override;
	void				RequestPreview( int id, int kind, const char * name, int size ) override;
	void				CancelPreviews() override { preview.Cancel(); }
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
	idEditorPreview		preview;
	bool				previewFrame = false;	// the frame being drawn is a preview picture
	int					previewPauseUntil = 0;	// no picture before this time, after one failed
	bool				lastWasPreview = false;	// so the edit view gets every other frame
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
	// Nothing paces the editor's frames (no game tics), and it has no use for a thousand of them a second.
	const int sinceLast = Sys_Milliseconds() - lastFrameMsec;
	if ( sinceLast >= 0 && sinceLast < EDIT_FRAME_MSEC ) {
		Sys_Sleep( EDIT_FRAME_MSEC - sinceLast );
	}
	lastFrameMsec = Sys_Milliseconds();

	inFrame = true;
	previewFrame = false;
	Sys_EngineFrame();
	inFrame = false;

	if ( previewFrame ) {
		previewFrame = false;
		idEditorPreview::job_t job;
		idList<byte> rgb;
		if ( preview.Finish( job, rgb ) ) {
			if ( job.id == PREVIEW_TEST_ID ) {
				// R_WriteTGA takes RGBA; its flipVertical false is the top row first
				idList<byte> rgba;
				rgba.SetNum( job.size * job.size * 4 );
				for ( int i = 0; i < job.size * job.size; i++ ) {
					rgba[i * 4 + 0] = rgb[i * 3 + 0];
					rgba[i * 4 + 1] = rgb[i * 3 + 1];
					rgba[i * 4 + 2] = rgb[i * 3 + 2];
					rgba[i * 4 + 3] = 255;
				}
				R_WriteTGA( "editor_preview.tga", rgba.Ptr(), job.size, job.size, false );
				common->Printf( "wrote editor_preview.tga, %s\n", job.name.c_str() );
			} else if ( job.id != PREVIEW_SWEEP_ID ) {
				ui->OnPreviewReady( job.id, job.size, job.size, rgb.Ptr() );
			}
		}
	}
}

void idEditorEngineLocal::Shutdown() {
	preview.Shutdown();
	Sys_EngineShutdown();
}

void idEditorEngineLocal::RequestPreview( int id, int kind, const char * name, int size ) {
	if ( !started || name == nullptr || name[0] == '\0' ) {
		return;
	}
	idEditorPreview::job_t job;
	job.id = id;
	job.kind = kind;
	job.name = name;
	job.size = size;
	preview.Request( job );
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

void idEditorEngineLocal::EnumAssets( int kind, editorAssetVisitor_t visitor, void * context ) {
	if ( !started ) {
		return;
	}
	if ( kind == EDITOR_ASSET_MODEL ) {
		// The models of the retail data exist only in their binary form, in the resource files:
		// generated/rendermodels/<name>.b<extension>. Loose source files are models too.
		idStrList names;
		fileSystem->ListResourceFiles( GENERATED_MODELS, names );
		for ( int i = 0; i < names.Num(); i++ ) {
			idStr & name = names[i];
			idStr extension;
			name.ExtractFileExtension( extension );
			name.StripLeading( GENERATED_MODELS );
			name.SetFileExtension( extension.c_str() + 1 );
		}
		std::unique_ptr<idFileList> loose( fileSystem->ListFilesTree( "models", ".lwo|.ase|.md5mesh|.ma" ) );
		for ( int i = 0; i < loose->GetNumFiles(); i++ ) {
			names.Append( loose->GetFile( i ) );
		}
		std::sort( names.Ptr(), names.Ptr() + names.Num(), []( const idStr & a, const idStr & b ) { return a.Icmp( b ) < 0; } );
		for ( int i = 0; i < names.Num(); i++ ) {
			if ( i == 0 || names[i].Icmp( names[i - 1] ) != 0 ) {
				visitor( context, names[i] );
			}
		}
		return;
	}
	declType_t type = DECL_MATERIAL;
	switch ( kind ) {
		case EDITOR_ASSET_MATERIAL:		type = DECL_MATERIAL; break;
		case EDITOR_ASSET_ENTITY_CLASS:	type = DECL_ENTITYDEF; break;
		case EDITOR_ASSET_SOUND:		type = DECL_SOUND; break;
		case EDITOR_ASSET_MAP:			type = DECL_MAPDEF; break;
		default:						return;
	}
	const int num = declManager->GetNumDecls( type );
	for ( int i = 0; i < num; i++ ) {
		// not parsed: parsing an entityDef caches its media
		visitor( context, declManager->DeclByIndex( type, i, false )->GetName() );
	}
}

// the text of a decl; the engine keeps it compressed
static void DeclText( const idDecl * decl, idList<char> & text ) {
	text.SetNum( decl->GetTextLength() + 1 );
	decl->GetText( text.Ptr() );
	text[decl->GetTextLength()] = '\0';
}

bool idEditorEngineLocal::GetMaterial( const char * name, editorMaterialVisitor_t visitor, void * context ) {
	const idMaterial * const material = started ? declManager->FindMaterial( name, false ) : nullptr;
	if ( material == nullptr ) {
		return false;
	}
	idList<char> text;
	DeclText( material, text );
	const bool implicit = material->IsImplicit();
	const editorMaterialInfo_t info = { text.Ptr(), implicit ? "" : material->GetFileName(), material->GetLineNum(), implicit, material->GetState() != DS_DEFAULTED };
	visitor( context, info );
	return true;
}

bool idEditorEngineLocal::ApplyMaterialText( const char * name, const char * text ) {
	// with com_smp 0 nothing of the last frame still reads the material between two frames
	if ( !started || inFrame ) {
		return false;
	}
	idMaterial * const material = const_cast<idMaterial *>( declManager->FindMaterial( name, false ) );
	if ( material == nullptr ) {
		return false;
	}
	material->SetText( text );
	material->Invalidate();
	material->EnsureNotPurged();
	return material->GetState() != DS_DEFAULTED;
}

bool idEditorEngineLocal::SaveMaterial( const char * name ) {
	if ( !started || inFrame ) {
		return false;
	}
	idMaterial * const material = const_cast<idMaterial *>( declManager->FindMaterial( name, false ) );
	return material != nullptr && material->ReplaceSourceFileText();
}

void idEditorEngineLocal::PlaySound( const char * name ) {
	idSoundWorld * const soundWorld = started ? soundSystem->GetPlayingSoundWorld() : nullptr;
	if ( soundWorld != nullptr ) {
		soundWorld->PlayShaderDirectly( name );
	}
}

void idEditorEngineLocal::CreateEntity( const char * classname, const char * model ) {
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
	if ( model != nullptr && model[0] != '\0' ) {
		change.after.Set( "model", model );
	}
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
	idEditorPreview::job_t failed;
	if ( preview.TakeFailed( failed ) ) {
		common->Printf( "No preview of '%s': the engine stopped its frame with an error.\n", failed.name.c_str() );
		if ( failed.id >= 0 ) {
			ui->OnPreviewReady( failed.id, 0, 0, nullptr );
		}
		// the engine makes a fatal error of several errors within 100 ms of each other
		previewPauseUntil = Sys_Milliseconds() + PREVIEW_ERROR_PAUSE_MSEC;
	}
	// a picture takes a frame of its own; with a map open, every other frame at most
	if ( preview.HasPending() && !( world.IsOpen() && lastWasPreview ) && Sys_Milliseconds() >= previewPauseUntil && preview.Draw() ) {
		previewFrame = true;
		lastWasPreview = true;
		return true;
	}
	lastWasPreview = false;
	if ( !world.IsOpen() ) {
		// an empty viewport; left to the engine, it would show the game selection screen
		renderSystem->SetColor4( EMPTY_VIEW_GREY, EMPTY_VIEW_GREY, EMPTY_VIEW_GREY, 1.0f );
		renderSystem->DrawStretchPic( 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 1.0f, 1.0f, declManager->FindMaterial( "_white" ) );
		renderSystem->SetColor4( 1.0f, 1.0f, 1.0f, 1.0f );
		return true;
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
	if ( args.Argc() < 2 ) {
		common->Printf( "usage: editorCreate <entityDef> [model]\n" );
		return;
	}
	if ( NeedEditor() ) {
		editorEngine->RunEdit( [&args] { editorEngine->CreateEntity( args.Argv( 1 ), ( args.Argc() > 2 ) ? args.Argv( 2 ) : nullptr ); } );
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

CONSOLE_COMMAND_SHIP( editorPreviewTest, "writes a preview picture to editor_preview.tga: editorPreviewTest material|model|class <name> [size]", nullptr ) {
	const char * const kinds[] = { "material", "model", "class" };
	int kind = -1;
	for ( int i = 0; i < 3; i++ ) {
		if ( idStr::Icmp( args.Argv( 1 ), kinds[i] ) == 0 ) {
			kind = i;
		}
	}
	if ( args.Argc() < 3 || kind == -1 ) {
		common->Printf( "usage: editorPreviewTest material|model|class <name> [size]\n" );
		return;
	}
	if ( NeedEditor() ) {
		editorEngine->RequestPreview( PREVIEW_TEST_ID, kind, args.Argv( 2 ), ( args.Argc() > 3 ) ? atoi( args.Argv( 3 ) ) : 256 );
	}
}

CONSOLE_COMMAND_SHIP( editorPreviewSweep, "makes the preview of every material, model or class and throws it away, a memory test: editorPreviewSweep material|model|class [first] [count]", nullptr ) {
	const int assetKinds[] = { EDITOR_ASSET_MATERIAL, EDITOR_ASSET_MODEL, EDITOR_ASSET_ENTITY_CLASS };
	const char * const kinds[] = { "material", "model", "class" };
	int kind = -1;
	for ( int i = 0; i < 3; i++ ) {
		if ( idStr::Icmp( args.Argv( 1 ), kinds[i] ) == 0 ) {
			kind = i;
		}
	}
	if ( kind == -1 || !NeedEditor() ) {
		common->Printf( "usage: editorPreviewSweep material|model|class [first] [count]\n" );
		return;
	}
	struct sweep_t {
		int		kind;
		int		first;
		int		count;
		int		seen;
		int		asked;
	} sweep = { kind, atoi( args.Argv( 2 ) ), ( args.Argc() > 3 ) ? atoi( args.Argv( 3 ) ) : INT_MAX, 0, 0 };
	editorEngine->EnumAssets( assetKinds[kind], []( void * context, const char * name ) {
		sweep_t & s = *static_cast<sweep_t *>( context );
		if ( s.seen++ >= s.first && s.asked < s.count ) {
			s.asked++;
			editorEngine->RequestPreview( PREVIEW_SWEEP_ID, s.kind, name, 128 );
		}
	}, &sweep );
	common->Printf( "editorPreviewSweep: %d pictures asked for\n", sweep.asked );
}

// every line of the block and of its blocks, the last one first, so that an edit moves no offset still needed
static void CollectLines( const mtr::block_t & block, std::vector<mtr::line_t> & lines ) {
	for ( const mtr::line_t & line : block.lines ) {
		lines.push_back( line );
	}
	for ( const mtr::block_t & child : block.blocks ) {
		CollectLines( child, lines );
	}
}

CONSOLE_COMMAND_SHIP( editorEditMaterial, "opens a material in the material editor", nullptr ) {
	if ( args.Argc() != 2 || !NeedEditor() ) {
		common->Printf( "usage: editorEditMaterial <material>\n" );
		return;
	}
	editorEngine->EditMaterial( args.Argv( 1 ) );
}

CONSOLE_COMMAND_SHIP( editorCheckMaterials, "parses the text of every material as the material editor does and writes every value back: the text must not change", nullptr ) {
	int checked = 0;
	int failed = 0;
	const int num = declManager->GetNumDecls( DECL_MATERIAL );
	for ( int i = 0; i < num; i++ ) {
		const idDecl * const material = declManager->DeclByIndex( DECL_MATERIAL, i, false );
		if ( material->IsImplicit() || material->GetTextLength() == 0 ) {
			continue;
		}
		idList<char> buffer;
		DeclText( material, buffer );
		const std::string text( buffer.Ptr() );
		checked++;

		mtr::decl_t decl;
		std::string error;
		if ( !mtr::ParseDecl( text, decl, error ) ) {
			common->Printf( "%s: %s\n", material->GetName(), error.c_str() );
			failed++;
			continue;
		}
		std::vector<mtr::line_t> lines;
		CollectLines( decl.body, lines );
		std::sort( lines.begin(), lines.end(), []( const mtr::line_t & a, const mtr::line_t & b ) { return a.begin > b.begin; } );
		std::string rewritten = text;
		for ( const mtr::line_t & line : lines ) {
			rewritten = mtr::WithValue( rewritten, line, std::string( mtr::Value( rewritten, line ) ) );
		}
		// the engine's name has no extension and no backslashes
		idStr name = decl.name.c_str();
		name.BackSlashesToSlashes();
		name.StripFileExtension();
		if ( rewritten != text ) {
			common->Printf( "%s: the text changed\n", material->GetName() );
			failed++;
		} else if ( name.Icmpn( material->GetName(), name.Length() ) != 0 ) {
			// not Icmp: the engine reads the retail 'textures/sfx/pane//1' as one name, comment and all
			common->Printf( "%s: the name is read as '%s'\n", material->GetName(), decl.name.c_str() );
			failed++;
		}
	}
	common->Printf( "editorCheckMaterials: %d materials checked, %d failed\n", checked, failed );
}

CONSOLE_COMMAND_SHIP( editorSetMaterialKey, "changes a key of a material's declaration as the material editor does: editorSetMaterialKey <material> <key> <value> [save]", nullptr ) {
	if ( args.Argc() < 4 || !NeedEditor() ) {
		common->Printf( "usage: editorSetMaterialKey <material> <key> <value> [save]\n" );
		return;
	}
	struct found_t {
		std::string		text;
	} found;
	if ( !editorEngine->GetMaterial( args.Argv( 1 ), []( void * context, const editorMaterialInfo_t & material ) {
			static_cast<found_t *>( context )->text = material.text;
		}, &found ) ) {
		common->Printf( "There is no material '%s'.\n", args.Argv( 1 ) );
		return;
	}
	mtr::decl_t decl;
	std::string error;
	if ( !mtr::ParseDecl( found.text, decl, error ) ) {
		common->Printf( "%s: %s\n", args.Argv( 1 ), error.c_str() );
		return;
	}
	const mtr::line_t * const line = mtr::FindLine( decl.body, args.Argv( 2 ) );
	std::string text;
	if ( line != nullptr ) {
		text = mtr::WithValue( found.text, *line, args.Argv( 3 ) );
	} else {
		// a new line, first in the body
		text = found.text.substr( 0, decl.body.open + 1 ) + va( "\n\t%s\t%s", args.Argv( 2 ), args.Argv( 3 ) ) + found.text.substr( decl.body.open + 1 );
	}
	bool ok = false;
	editorEngine->RunEdit( [&] { ok = editorEngine->ApplyMaterialText( args.Argv( 1 ), text.c_str() ); } );
	common->Printf( "editorSetMaterialKey: %s %s\n", args.Argv( 1 ), ok ? "applied" : "NOT applied" );
	if ( ok && idStr::Icmp( args.Argv( 4 ), "save" ) == 0 ) {
		editorEngine->RunEdit( [&] { ok = editorEngine->SaveMaterial( args.Argv( 1 ) ); } );
		common->Printf( "editorSetMaterialKey: %s %s\n", args.Argv( 1 ), ok ? "saved" : "NOT saved" );
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
