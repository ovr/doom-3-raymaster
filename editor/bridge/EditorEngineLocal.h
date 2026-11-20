// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <string>

#include "framework/EngineHost.h"

#include "../api/EditorEngineApi.h"

#include "EditorHistory.h"
#include "EditorPreview.h"
#include "EditorTool.h"
#include "EditorView.h"
#include "EditorWorld.h"

/*
	The engine as the UI sees it, and the UI as the engine sees it. Its parts by file:

	  EditorEngine.cpp        the frame, maps, entities, materials, previews, Play
	  EditorEngineBrush.cpp   the brush selection and the brush edits
	  EditorEngineBuild.cpp   Build: save, compile, show the result or the leak
	  EditorEngineView.cpp    the tools' state, what a 2D view draws, the camera

	Needs <windows.h> first.
*/
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
	void				NewMap( const char * name ) override;
	[[nodiscard]] bool	MapExists( const char * name ) const override;
	void				CloseMap() override;
	[[nodiscard]] const char *	CurrentMap() const override { return world.Name(); }
	void				EnumEntities( editorEntityVisitor_t visitor, void * context ) override;
	void				LookAtEntity( int index ) override;
	void				SelectEntity( int index, bool add ) override;
	void				EnumSelection( editorSelectionVisitor_t visitor, void * context ) override;
	void				SetGizmo( int gizmo ) override;
	void				SetGridSize( float size ) override;
	void				SetTool( int tool ) override;
	void				SetBoxHeight( float height ) override;
	void				SetAngleSnap( float degrees ) override;
	void				ShowGrid( bool show ) override;
	void				MoveSelection( const float delta[3] ) override;
	void				EnumBrushEntityClasses( editorClassVisitor_t visitor, void * context ) override;
	bool				EnumViewBrushes( const float mins[3], const float maxs[3], int maxBrushes, editorViewBrushVisitor_t visitor, void * context ) override;
	void				EnumViewEntities( const float mins[3], const float maxs[3], editorViewEntityVisitor_t visitor, void * context ) override;
	void				GetCamera( editorCamera_t & camera ) const override;
	void				GoToLeak() override;
	[[nodiscard]] bool	Pick( int x, int y, editorPickInfo_t & pick ) override;
	void				SelectBrush( int entityIndex, int primitive, int face, bool add ) override;
	void				EnumBrushSelection( editorBrushVisitor_t visitor, void * context ) override;
	void				EnumBrushFaces( int entityIndex, int primitive, editorFaceVisitor_t visitor, void * context ) override;
	void				CreateBrush( const float mins[3], const float maxs[3], const char * material ) override;
	void				MoveBrushes( const float delta[3] ) override;
	void				MoveFace( float distance ) override;
	void				RotateBrushes( int axis, float degrees ) override;
	void				SetFaceMaterial( const char * material ) override;
	void				SetFaceTexture( const editorFaceTexture_t & texture ) override;
	void				HollowBrushes( float thickness ) override;
	void				MakeBrushEntity( const char * classname ) override;
	void				MoveBrushesToWorld() override;
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
	void				Build( bool fast ) override { cmdSystem->BufferCommandText( CMD_EXEC_APPEND, fast ? "editorBuild fast\n" : "editorBuild\n" ); }
	void				CancelBuild() override { buildCancel = true; }
	void				ShowLeak( bool show ) override;
	void				Play() override { cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "editorPlay\n" ); }
	void				StopPlay() override { cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "editorStop\n" ); }

	// the console commands behind the queued calls
	void				OpenMapNow( const char * name );
	void				NewMapNow( const char * name );
	void				CloseMapNow();
	void				UndoNow();
	void				RedoNow();
	void				SaveMapNow();
	void				PlayNow();
	void				StopNow();
	// true when the viewport shows the compiled world afterwards
	bool				BuildNow( bool fast, bool cancelTest = false );
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
	// -1 and a print when the open map has no such entity; "world" and "0" are the worldspawn
	[[nodiscard]] int	FindEntity( const char * entityName ) const;

	// for the console: what a script needs to see and to steer without a mouse
	void				PrintWorldInfo() const;
	void				PrintBrushInfo() const;
	void				PrintPatches( int max ) const;
	void				SetView( const idVec3 & origin, const idAngles & angles ) { view.SetView( origin, angles ); }
	// the edit view's next frame goes into a TGA file in fs_savepath
	void				ViewShot( const char * fileName ) { viewShotName = fileName; }

	// idEditorViewClient
	void				ViewMouseDown( int x, int y, bool ctrl, bool shift ) override;
	void				ViewMouseMove( int x, int y ) override;
	void				ViewMouseUp() override;

	// idEngineHost
	void *				ParentWindow() override { return ui->ViewportParent(); }
	void				GetViewportSize( int & width, int & height ) override;
	void				PumpEvents() override;
	void				OnQuitRequested() override { ui->OnQuitRequested(); }
	void				OnFatalError( const char * text ) override { ui->OnFatalError( text ); }
	void				OnPrint( const char * text ) override { ui->OnPrint( text ); }
	void				OnEscapeToHost() override;
	bool				DrawScene() override;

private:
	using brushRef_t = idEditorTool::brushRef_t;

	[[nodiscard]] bool	IsEntity( int index ) const { return index > 0 && index < world.NumEntities() && !world.Entity( index ).removed; }
	// no edit while the engine runs a frame, a drag holds the entities or a build runs
	[[nodiscard]] bool	CanEdit() const { return world.IsOpen() && !playing && !building && !inFrame && !tool.Dragging(); }
	// after a frame while playing: has the game started, has it ended
	void				UpdatePlay();
	void				EndPlay();
	// applies the changes as one step of the history and tells the UI
	void				Edit( const char * label, const idList<editorChange_t> & changes );
	void				Edit( const char * label, const idList<editorChange_t> & changes, const idList<editorPrimitiveChange_t> & primitiveChanges );
	void				AfterEdit( bool listChanged );
	void				SelectOnly( const idList<int> & indices );
	void				SelectBrushesOnly( const idList<brushRef_t> & brushes );
	void				DuplicateBrushes();
	void				DeleteBrushes();
	// The brushes an edit works on, the last primitive of an entity first, so that removing one
	// moves none of the others; the selected patches too for the edits a patch takes.
	// false and a print when there is none.
	[[nodiscard]] bool	BrushesToEdit( idList<brushRef_t> & brushes, bool patchesToo = false ) const;
	// "brush", "3 patches", "4 primitives": what the list holds, for the history's labels
	[[nodiscard]] idStr	PrimitivesLabel( const char * verb, const idList<brushRef_t> & primitives ) const;
	[[nodiscard]] float	Snap( float value ) const;
	[[nodiscard]] HWND	Viewport() const;
	void				WriteViewShot();
	void				BuildProgress( const char * part, const char * stage, float fraction );
	// lets the UI repaint and cancel, not more often than it can be seen
	void				PumpBuild();

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
	bool				playing = false;		// from editorPlay until the game has ended
	bool				playSpawned = false;	// the game's map has run
	int					playFrames = 0;
	bool				building = false;
	bool				buildCancel = false;
	int					lastBuildPumpMsec = 0;
	idStr				viewShotName;
	idStrList			brushClasses;			// EnumBrushEntityClasses(), found once
};

// for the console commands; nullptr in the bare shell
extern idEditorEngineLocal *	editorEngine;

// false and a print in the bare shell
[[nodiscard]] bool	Editor_NeedEngine();
