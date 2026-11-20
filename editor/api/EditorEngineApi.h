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

constexpr int EDITOR_API_VERSION = 11;

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

// what the left button does in the viewport
enum editorTool_t : int {
	EDITOR_TOOL_SELECT,			// click selects, the gizmo moves or turns
	EDITOR_TOOL_BOX				// a drag draws a rectangle that becomes a brush
};

// the state of the open map's history, for the Edit menu; the labels are "" when there is no step
struct editorDocumentState_t {
	bool				dirty;			// differs from the file, or a new map that was never saved
	bool				playing;		// the game runs the map: nothing can be edited
	bool				canUndo;
	bool				canRedo;
	const char *		undoLabel;
	const char *		redoLabel;
	// The viewport shows the brushes and patches of the document, not a compiled world: a primitive
	// was edited since the last build, or the map has no compiled world. A build clears it.
	bool				geometryDirty;
	bool				building;		// inside Build(): only CancelBuild() does anything
	int					leakPoints;		// of the last build's leak line, 0 = none
	bool				leakShown;
	float				gridSize;		// 0 = no snapping
	float				angleSnap;		// degrees a turn snaps to, 0 = none
	bool				gridShown;		// the grid of the 3D viewport
	int					tool;			// editorTool_t
	float				boxHeight;		// how high the box tool's brush is, as it is now (never 0)
};

// how a 2D view draws a primitive
enum editorViewBrushKind_t : int {
	EDITOR_VIEW_BRUSH_DRAWN,
	EDITOR_VIEW_BRUSH_HIDDEN,	// caulk, nodraw
	EDITOR_VIEW_BRUSH_PORTAL,
	EDITOR_VIEW_BRUSH_CLIP,
	EDITOR_VIEW_BRUSH_TRIGGER,
	EDITOR_VIEW_PATCH			// the edges are its control net; selected and edited as a whole like a brush
};

// A brush or a patch for a 2D view, in world coordinates. edges holds numEdges lines, six floats
// each (two points); valid during the call only.
struct editorViewBrush_t {
	int					entity;			// 0 = the worldspawn
	int					primitive;
	int					kind;			// editorViewBrushKind_t
	bool				selected;		// the brush, or its entity
	float				mins[3];
	float				maxs[3];
	int					numEdges;
	const float *		edges;
};
using editorViewBrushVisitor_t = void ( * )( void * context, const editorViewBrush_t & brush );

// an entity for a 2D view: its box in the world; one with primitives is drawn by its brushes
struct editorViewEntity_t {
	int					index;
	bool				isLight;
	bool				selected;
	bool				hasPrimitives;
	float				origin[3];
	float				mins[3];
	float				maxs[3];
};
using editorViewEntityVisitor_t = void ( * )( void * context, const editorViewEntity_t & entity );

// the edit view's camera
struct editorCamera_t {
	float				origin[3];
	float				forward[3];
};

// how a build ended
enum editorBuildStatus_t : int {
	EDITOR_BUILD_OK,
	EDITOR_BUILD_LEAKED,		// an entity sees the outside: nothing was compiled, the leak line is shown
	EDITOR_BUILD_CANCELLED,
	EDITOR_BUILD_FAILED			// see error
};

// valid during OnBuildFinished() only
struct editorBuildResult_t {
	int					status;			// editorBuildStatus_t
	const char *		error;			// "" unless the status is FAILED
	int					areas;			// of the world
	int					portals;		// between the areas
	int					triangles;		// of all models
	int					aasFiles;		// written; 0 for a map without monsters and for a fast build
	int					aasAreas;		// of all files
	int					leakPoints;
	float				seconds;
};

// What is under a pixel of the viewport. entity -1: nothing. A primitive is a brush or a patch of
// the entity (0 = the worldspawn); face is the side of a brush, -1 for a patch.
struct editorPickInfo_t {
	int					entity;
	int					primitive;
	int					face;			// -1 for a patch and for an entity without primitives
	bool				isPatch;
	float				distance;
	float				point[3];
};

// The texture of a brush side: shift in repeats, repeat in world units per repeat of the texture
// along S and T (negative repeatT: mirrored), rotate in degrees.
struct editorFaceTexture_t {
	float				shiftS;
	float				shiftT;
	float				repeatS;
	float				repeatT;
	float				rotate;
};

// A patch is listed as one face: its material is all of it that can be set.
struct editorFaceInfo_t {
	int					face;
	bool				isPatch;
	int					patchWidth;		// control points; 0 for a brush side
	int					patchHeight;
	const char *		material;
	float				normal[3];		// out of the brush, in the space of its entity
	float				dist;			// normal · x = dist
	bool				visible;		// false: the other sides cut it away
	editorFaceTexture_t	texture;
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
	int					numPrimitives;	// brushes and patches
};

// the strings are valid during the call only
using editorMapVisitor_t = void ( * )( void * context, const editorMapInfo_t & map );
using editorEntityVisitor_t = void ( * )( void * context, const editorEntityInfo_t & entity );
using editorSelectionVisitor_t = void ( * )( void * context, int entityIndex );
// face -1: the whole brush
using editorBrushVisitor_t = void ( * )( void * context, int entityIndex, int primitive, int face );
using editorFaceVisitor_t = void ( * )( void * context, const editorFaceInfo_t & face );

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

// A material's declaration: text is all of it, "[type] name { ... }", as the engine has it now.
struct editorMaterialInfo_t {
	const char *		text;
	const char *		fileName;		// "materials/base_wall.mtr"; "" for an implicit material
	int					line;
	bool				implicit;		// made by the engine from a texture name: it has no file
	bool				valid;			// the text parsed without an error
};
using editorMaterialVisitor_t = void ( * )( void * context, const editorMaterialInfo_t & material );

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

	// Inside Build(), which blocks its frame: the stage that runs ("map: bsp", "aas48: reachability")
	// and how far it is, 0 to 1. PumpEvents() is called along with it.
	virtual void		OnBuildProgress( const char * stage, float fraction ) = 0;
	// the build ended; OnDocumentChanged() follows
	virtual void		OnBuildFinished( const editorBuildResult_t & result ) = 0;

	// one line for the status bar: why a click did nothing ("a patch cannot be edited")
	virtual void		OnStatus( const char * text ) = 0;

	// the console asked for a material in the material editor (editorEditMaterial)
	virtual void		OnEditMaterial( const char * name ) = 0;

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
	// A map of a worldspawn and nothing else, "editor/room1"; no file is written before SaveMap().
	virtual void		NewMap( const char * name ) = 0;
	// a maps/<name>.map exists, in the game data or in the project folder
	[[nodiscard]] virtual bool	MapExists( const char * name ) const = 0;
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
	// also what new brushes, brush moves and face moves snap to
	virtual void		SetGridSize( float size ) = 0;
	// editorTool_t; ends in OnDocumentChanged()
	virtual void		SetTool( int tool ) = 0;
	// how high the box tool makes its brush; 0 = eight steps of the grid
	virtual void		SetBoxHeight( float height ) = 0;
	// the step of the rotate gizmo, for entities and brushes; 0 = none
	virtual void		SetAngleSnap( float degrees ) = 0;
	// the grid of the 3D viewport, on the plane z = 0 or the one the box tool drew on last
	virtual void		ShowGrid( bool show ) = 0;

	/*
		Brushes. The selection is either entities or brushes, never both:

		  click on a brush of the worldspawn    selects the brush; Ctrl+click adds or removes one
		  click on the one selected brush       selects the face under the cursor
		  Shift+click on a brush                selects the brush and that face at once
		  click on a brush entity               selects the entity (SelectBrush() reaches its brushes)

		With brushes selected the move gizmo moves them and the rotate gizmo turns them; with a face
		selected it is one arrow along the normal that moves the face. With EDITOR_TOOL_BOX a drag
		draws a rectangle on the face or the grid plane (z = 0) under the cursor and letting go
		makes a brush of it.

		A patch of the worldspawn is selected like a brush, with the same calls (it has no faces):
		it is moved, turned, duplicated, deleted and given a material as a whole. Its control
		points are not edited, and no call makes a new one.

		The edits work on the selected brushes and are one step of the history each. They do
		nothing while GetDocumentState() says playing or building. An edit makes geometryDirty.
		The index of a primitive is its place in its entity: deleting one moves the ones behind
		it, so ask EnumBrushSelection() again after OnSelectionChanged().
	*/
	// false: nothing there. x and y are pixels of the viewport.
	[[nodiscard]] virtual bool	Pick( int x, int y, editorPickInfo_t & pick ) = 0;
	// face -1 selects the whole brush; add = true adds the brush or removes it
	virtual void		SelectBrush( int entityIndex, int primitive, int face, bool add ) = 0;
	virtual void		EnumBrushSelection( editorBrushVisitor_t visitor, void * context ) = 0;
	virtual void		EnumBrushFaces( int entityIndex, int primitive, editorFaceVisitor_t visitor, void * context ) = 0;
	// An axial box in world coordinates, snapped to the grid, added to the selected brush entity or
	// else to the worldspawn, and selected. material nullptr: the default one.
	virtual void		CreateBrush( const float mins[3], const float maxs[3], const char * material ) = 0;
	virtual void		MoveBrushes( const float delta[3] ) = 0;
	// pushes the selected face out along its normal, or pulls it in (negative)
	virtual void		MoveFace( float distance ) = 0;
	// Turns the selected brushes and patches around the middle of the selection (on the grid), axis
	// 0..2 = x, y, z. Multiples of 90 degrees keep the planes exact; the textures turn with the brushes.
	virtual void		RotateBrushes( int axis, float degrees ) = 0;
	// the selected face, or every face of the selected brushes when no face is
	virtual void		SetFaceMaterial( const char * material ) = 0;
	virtual void		SetFaceTexture( const editorFaceTexture_t & texture ) = 0;
	// every selected brush becomes one wall brush for each of its faces
	virtual void		HollowBrushes( float thickness ) = 0;
	// the selected brushes of the worldspawn become a new entity of the class (func_static, func_door)
	virtual void		MakeBrushEntity( const char * classname ) = 0;
	// the primitives of the selected brush entities go to the worldspawn, the entities are removed
	virtual void		MoveBrushesToWorld() = 0;
	// The selection, brushes or entities (their "origin"), moved as one step; snapped to the grid.
	virtual void		MoveSelection( const float delta[3] ) = 0;
	// the entityDefs that are made of brushes (func_static first), for MakeBrushEntity()
	virtual void		EnumBrushEntityClasses( editorClassVisitor_t visitor, void * context ) = 0;

	// For a 2D view: the primitives and the entities that touch the box, in world coordinates.
	// At most maxBrushes primitives are visited; false when there were more.
	virtual bool		EnumViewBrushes( const float mins[3], const float maxs[3], int maxBrushes, editorViewBrushVisitor_t visitor, void * context ) = 0;
	virtual void		EnumViewEntities( const float mins[3], const float maxs[3], editorViewEntityVisitor_t visitor, void * context ) = 0;
	virtual void		GetCamera( editorCamera_t & camera ) const = 0;

	// The keys of an entity: the map's own first, then the ones its entityDef documents.
	virtual void		EnumEntityKeys( int index, editorKeyVisitor_t visitor, void * context ) = 0;
	// One step of the history. value nullptr takes the key out of the map: the default is back.
	virtual void		SetEntityKey( int index, const char * key, const char * value ) = 0;

	// every entityDef, for the palette
	virtual void		EnumEntityClasses( editorClassVisitor_t visitor, void * context ) = 0;
	// Makes an entity in front of the camera and selects it; model, when not nullptr, is its "model" key.
	virtual void		CreateEntity( const char * classname, const char * model ) = 0;
	// The selection, entities or brushes; brush entities are not duplicated, the copies become
	// the selection.
	virtual void		DuplicateSelection() = 0;
	virtual void		DeleteSelection() = 0;

	// false when there is no material of the name
	[[nodiscard]] virtual bool	GetMaterial( const char * name, editorMaterialVisitor_t visitor, void * context ) = 0;
	// Makes text the material's declaration; everything drawn with it changes at once, no file does.
	// false: the engine is inside a frame, there is no such material, or the text has an error
	// (then the material is the default one until the next good text).
	[[nodiscard]] virtual bool	ApplyMaterialText( const char * name, const char * text ) = 0;
	// Writes the material's text into its .mtr file in the project folder (fs_savepath).
	[[nodiscard]] virtual bool	SaveMaterial( const char * name ) = 0;

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

	// Saves the map and compiles it: the world (.proc, .cm) and, unless fast or the map has no
	// monster, the AAS files. Happens in the next frame and blocks it; the host gets
	// OnBuildProgress() and PumpEvents() meanwhile and OnBuildFinished() at the end. After a good
	// build the viewport shows the compiled world again.
	virtual void		Build( bool fast ) = 0;
	// ends the build that runs; safe from PumpEvents()
	virtual void		CancelBuild() = 0;
	// the leak line of the last build that leaked
	virtual void		ShowLeak( bool show ) = 0;
	// puts the camera at the start of the leak line, looking along it
	virtual void		GoToLeak() = 0;

	// Play in Editor: saves the open map when it has changes, builds it when geometryDirty, and
	// starts the game on it, in the viewport. A build that fails or leaks starts no game.
	// StopPlay(), Shift+Esc or the game's own exit ends it and the map is edited again.
	// The host hears of both through OnDocumentChanged().
	virtual void		Play() = 0;
	virtual void		StopPlay() = 0;

protected:
						~idEditorEngine() = default;
};

// the UI module's only export; returns when the UI could not start
using editorUiMain_t = int ( * )( idEditorEngine * engine, int argc, char ** argv );
constexpr char EDITOR_UI_MAIN_NAME[] = "EditorUiMain";
constexpr char EDITOR_UI_MODULE_PATH[] = "editor\\Doom3EditorUI.dll";
