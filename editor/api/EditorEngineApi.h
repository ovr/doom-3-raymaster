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

constexpr int EDITOR_API_VERSION = 6;

// what the content browser lists
enum editorAssetKind_t : int {
	EDITOR_ASSET_MATERIAL,
	EDITOR_ASSET_MODEL,			// a model file: "models/mapobjects/.../x.lwo"
	EDITOR_ASSET_ENTITY_CLASS,
	EDITOR_ASSET_SOUND,			// a sound shader
	EDITOR_ASSET_MAP,
	EDITOR_ASSET_KINDS
};

// what a preview picture shows; a material is put on a sphere
enum editorPreviewKind_t : int {
	EDITOR_PREVIEW_MATERIAL,
	EDITOR_PREVIEW_MODEL,
	EDITOR_PREVIEW_ENTITY_CLASS
};

// what a drag in the viewport does to the selection
enum editorGizmo_t : int {
	EDITOR_GIZMO_MOVE,
	EDITOR_GIZMO_ROTATE
};

// the state of the open map's history, for the Edit menu; the labels are "" when there is no step
struct editorDocumentState_t {
	bool				dirty;
	bool				canUndo;
	bool				canRedo;
	const char *		undoLabel;
	const char *		redoLabel;
};

// a map the game data declares (a mapDef): name is what OpenMap takes, "game/mc_underground"
struct editorMapInfo_t {
	const char *		name;
	const char *		title;
};

// an entity of the open map; index 0 is the worldspawn
struct editorEntityInfo_t {
	int					index;
	const char *		name;
	const char *		classname;
	bool				isLight;
	bool				hasModel;
};

// the strings are valid during the call only
using editorMapVisitor_t = void ( * )( void * context, const editorMapInfo_t & map );
using editorEntityVisitor_t = void ( * )( void * context, const editorEntityInfo_t & entity );
using editorSelectionVisitor_t = void ( * )( void * context, int entityIndex );

// how the inspector edits a key
enum editorKeyType_t : int {
	EDITOR_KEY_TEXT,
	EDITOR_KEY_BOOL,
	EDITOR_KEY_COLOR		// "r g b", each 0 .. 1
};

// A key of an entity. inMap: the map file sets it; otherwise value is the entityDef's default.
struct editorKeyInfo_t {
	const char *		key;
	const char *		value;
	const char *		description;	// from the entityDef's editor_* keys, "" when it has none
	int					type;			// editorKeyType_t
	bool				inMap;
};
using editorKeyVisitor_t = void ( * )( void * context, const editorKeyInfo_t & key );
using editorClassVisitor_t = void ( * )( void * context, const char * classname );
using editorAssetVisitor_t = void ( * )( void * context, const char * name );

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

	// a map was opened or closed: CurrentMap() and the entities are new
	virtual void		OnMapChanged() = 0;

	// entities were made, removed or renamed: EnumEntities() is new
	virtual void		OnEntitiesChanged() = 0;
	// the selection changed, in the viewport or through SelectEntity()
	virtual void		OnSelectionChanged() = 0;
	// an edit, an undo, a redo or a save: GetDocumentState() is new
	virtual void		OnDocumentChanged() = 0;

	// A picture asked for with RequestPreview(): RGB8, tightly packed, top row first, valid during
	// the call. It can be smaller than asked when the viewport is. width 0 and rgb nullptr: the
	// engine could not draw it (an error in the asset).
	virtual void		OnPreviewReady( int id, int width, int height, const unsigned char * rgb ) = 0;

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

	// The map being edited. Opening and closing happen in the next frame and end in OnMapChanged().
	// While a map is open the viewport is the edit view: hold the right button to look and fly.
	virtual void		EnumMaps( editorMapVisitor_t visitor, void * context ) = 0;
	virtual void		OpenMap( const char * name ) = 0;
	virtual void		CloseMap() = 0;
	// "" when no map is open
	[[nodiscard]] virtual const char *	CurrentMap() const = 0;
	virtual void		EnumEntities( editorEntityVisitor_t visitor, void * context ) = 0;
	// puts the camera in front of the entity
	virtual void		LookAtEntity( int index ) = 0;

	// The selection. In the viewport: a click selects, Ctrl+click adds or removes.
	// add = false replaces the selection; index 0 or less with add = false clears it.
	virtual void		SelectEntity( int index, bool add ) = 0;
	virtual void		EnumSelection( editorSelectionVisitor_t visitor, void * context ) = 0;

	// the gizmo at the selection (editorGizmo_t) and the step of a move, 0 = no snapping
	virtual void		SetGizmo( int gizmo ) = 0;
	virtual void		SetGridSize( float size ) = 0;

	// The keys of an entity: the map's own first, then the ones its entityDef documents.
	virtual void		EnumEntityKeys( int index, editorKeyVisitor_t visitor, void * context ) = 0;
	// One step of the history. value nullptr takes the key out of the map: the default is back.
	virtual void		SetEntityKey( int index, const char * key, const char * value ) = 0;

	// every entityDef, for the palette
	virtual void		EnumEntityClasses( editorClassVisitor_t visitor, void * context ) = 0;
	// Makes an entity in front of the camera and selects it; model, when not nullptr, is its "model" key.
	virtual void		CreateEntity( const char * classname, const char * model ) = 0;
	// The selection; brush entities are not duplicated, the copies become the selection.
	virtual void		DuplicateSelection() = 0;
	virtual void		DeleteSelection() = 0;

	// The names of the game data's assets of a kind (editorAssetKind_t), in no order.
	virtual void		EnumAssets( int kind, editorAssetVisitor_t visitor, void * context ) = 0;
	// plays a sound shader once, as the menus do; "" stops it
	virtual void		PlaySound( const char * name ) = 0;

	// Queues a picture of size x size pixels; it arrives in OnPreviewReady() with the id, one
	// picture a frame, in the order asked. The viewport has to be visible and at least that big.
	virtual void		RequestPreview( int id, int kind, const char * name, int size ) = 0;
	// forgets the pictures not made yet
	virtual void		CancelPreviews() = 0;

	// Undo, Redo and SaveMap happen in the next frame and end in OnDocumentChanged().
	// The map is saved into the project folder (fs_savepath), never into the game install.
	virtual void		GetDocumentState( editorDocumentState_t & state ) const = 0;
	virtual void		Undo() = 0;
	virtual void		Redo() = 0;
	virtual void		SaveMap() = 0;

protected:
						~idEditorEngine() = default;
};

// the UI module's only export; returns when the UI could not start
using editorUiMain_t = int ( * )( idEditorEngine * engine, int argc, char ** argv );
constexpr char EDITOR_UI_MAIN_NAME[] = "EditorUiMain";
constexpr char EDITOR_UI_MODULE_PATH[] = "editor\\Doom3EditorUI.dll";
