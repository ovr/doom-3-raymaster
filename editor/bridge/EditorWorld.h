// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <memory>

#include "EditorBrush.h"

/*
	The map being edited. Not a running game: the map file as it is on disk, and a render world of
	its own built from it.

	  maps/<name>.map ── idMapFile (asWritten) ── every entity's spawn args
	                                                    │ + the entityDef's defaults
	  maps/<name>.bproc ── idRenderWorld::InitFromMap   ▼
	                              │          idGameEdit::ParseSpawnArgsToRenderEntity / RenderLight
	                              └──────────── AddEntityDef / AddLightDef ── RenderScene( camera )

	So an entity of any class shows up without being spawned, and a changed key is one
	UpdateEntityDef away. What a script or the AI would do at run time is not shown.

	The geometry has two looks. While the compiled world (.proc) fits the document, that is shown.
	A changed primitive, or no compiled world, makes the geometry a preview: an empty render world
	and one render model for every brush and patch, made from the document (EditorBrush.h).

	  compiled:  InitFromMap( map )      areas, portals, the brush entities' models of the .proc
	  preview:   InitFromMap( nullptr )  one area; models of the primitives, outlines for portals,
	                                     clip and trigger brushes; a light at the camera
	  primitive edit ──► preview ──────── a successful build (ShowCompiled) ──► compiled

	The map file is the document: SetMapArgs() and ApplyPrimitive() are the only ways it changes,
	and Save() writes the file into fs_savepath, never into the game install.

	An entity keeps its index for as long as the map is open. A removed one stays in the list
	without key/values, so an undo can bring it back, and is not written. A primitive's index is
	its place in its entity: inserting or removing one moves the ones behind it.
*/

// One primitive of an entity changes: before nullptr = it is inserted at the index, after nullptr
// = it is removed, both = replaced. The primitives are shared with the history, never changed.
struct editorPrimitiveChange_t {
	int					entity = 0;
	int					primitive = 0;
	std::shared_ptr<const idMapPrimitive>	before;
	std::shared_ptr<const idMapPrimitive>	after;
};

// how a primitive is shown while the geometry is a preview
struct editorPrimitiveView_t {
	editorBrushKind_t	kind = editorBrushKind_t::HIDDEN;
	idBounds			bounds;				// in the entity's space; cleared for a broken brush
	idList<idVec3>		outline;			// pairs of points, for the kinds drawn as lines
	int					cell = -1;			// of the entity's cells; -1: nothing of it is drawn
};

// The drawn primitives of an entity whose middle is in one cube of space share a render model:
// a retail map has ten thousand primitives, a render world room for ten thousand entities.
struct editorGeometryCell_t {
	int					key[3] = {};
	idRenderModel *		model = nullptr;	// nullptr: nothing is left in it
	qhandle_t			handle = -1;
	bool				dirty = false;		// a view came or went: BuildCells() makes the model again
};

struct editorMapEntity_t {
	idDict				args;				// the map's own key/values over the entityDef's
	renderEntity_t		renderEntity;		// valid while modelHandle != -1
	renderLight_t		renderLight;		// valid while lightHandle != -1
	qhandle_t			modelHandle = -1;
	qhandle_t			lightHandle = -1;
	idJointMat *		joints = nullptr;	// the pose of a skinned model, Mem_Alloc16
	idBounds			bounds;				// around the origin, for the entities nothing draws
	idVec3				origin = vec3_origin;
	idMat3				axis = mat3_identity;
	bool				isLight = false;
	bool				removed = false;
	// the preview of its primitives, one for each; nullptr while the compiled world is shown
	idList<editorPrimitiveView_t> *	views = nullptr;
	idList<editorGeometryCell_t> *	cells = nullptr;
	idBounds			viewBounds;			// of all views, in the entity's space
};

// what a ray hits: an entity, and for a brush or patch its primitive and the brush's side
struct editorPick_t {
	int					entity = -1;		// -1 = nothing; 0 = the worldspawn, with a primitive
	int					primitive = -1;
	int					face = -1;
	float				distance = 0.0f;
};

class idEditorWorld {
public:
							~idEditorWorld() { Close(); }

	// name as in a mapDef: "game/mc_underground"
	[[nodiscard]] bool	Open( const char * name );
	// a document of a worldspawn without primitives; nothing is on disk until Save()
	void				New( const char * name );
	void				Close();
	[[nodiscard]] bool	IsOpen() const { return !mapName.IsEmpty(); }
	// While a game plays the map: the document stays, the render world goes, because the game's
	// level load purges the models and images it holds. Resume() builds it again from the document.
	void				Suspend();
	[[nodiscard]] bool	Resume();
	[[nodiscard]] bool	IsSuspended() const { return IsOpen() && world == nullptr; }
	[[nodiscard]] const char *	Name() const { return mapName.c_str(); }
	// into fs_savepath/maps
	[[nodiscard]] bool	Save();
	// a new map that was never written
	[[nodiscard]] bool	IsUnsaved() const { return unsaved; }

	// The geometry shown is the preview, not a compiled world.
	[[nodiscard]] bool	IsPreview() const { return previewGeometry; }
	// After a build: loads the .proc and shows it. false: there is none, the preview stays.
	[[nodiscard]] bool	ShowCompiled();

	[[nodiscard]] int	NumEntities() const { return entities.Num(); }
	[[nodiscard]] const editorMapEntity_t &	Entity( int index ) const { return entities[index]; }

	// the key/values as the map file has them, without the entityDef's defaults
	[[nodiscard]] const idDict &	MapArgs( int index ) const { return mapFile.GetEntity( index )->epairs; }
	// replaces them and shows the entity as they now describe it; no key/values remove the entity
	void				SetMapArgs( int index, const idDict & mapArgs );
	// a new, removed entity for SetMapArgs() to fill
	[[nodiscard]] int	AddEntity();
	[[nodiscard]] bool	HasPrimitives( int index ) const { return mapFile.GetEntity( index )->GetNumPrimitives() > 0; }
	// -1 when no entity has the name
	[[nodiscard]] int	FindEntity( const char * name ) const;
	// "<base>_<n>" that no entity has yet
	[[nodiscard]] idStr	UniqueName( const char * base ) const;

	[[nodiscard]] int	NumPrimitives( int entity ) const { return mapFile.GetEntity( entity )->GetNumPrimitives(); }
	// nullptr when the entity has no such primitive, or it is not a brush
	[[nodiscard]] const idMapPrimitive *	Primitive( int entity, int primitive ) const;
	[[nodiscard]] const idMapBrush *	Brush( int entity, int primitive ) const;
	[[nodiscard]] const idMapPatch *	Patch( int entity, int primitive ) const;
	// what a primitive covers in the world; cleared when there is none or it is a broken brush
	[[nodiscard]] idBounds	PrimitiveWorldBounds( int entity, int primitive ) const;
	// Does the change (forward) or takes it back. The geometry becomes the preview.
	void				ApplyPrimitive( const editorPrimitiveChange_t & change, bool forward );
	// The box of every primitive of an entity, in the entity's space; cleared for a brush without
	// a volume. Kept between calls and through edits: a 2D view asks often.
	[[nodiscard]] const idList<idBounds> &	PrimitiveBounds( int entity ) const;
	// the brush's faces in the world; false for a brush without a volume
	[[nodiscard]] bool	BrushWindings( int entity, int primitive, editorBrushWindings_t & windings ) const;

	// what the entity covers in the world: its model, its primitives, or its marker box
	[[nodiscard]] idBounds	WorldBounds( int index ) const;
	// The nearest thing on the ray. Only what is seen can be hit: a model, a marker box, a drawn
	// side of a brush, a drawn patch, and in the preview a brush drawn as an outline.
	[[nodiscard]] editorPick_t	Pick( const idVec3 & start, const idVec3 & dir, float maxDistance ) const;

	// where a new view starts: the player start, or the middle of the entities
	void				GetStartView( idVec3 & origin, idAngles & angles ) const;

	// the line from an entity to the outside that a leaking map's compile wrote; false: no file
	[[nodiscard]] bool	LoadLeak( const char * fileName );
	void				ClearLeak() { leakPoints.Clear(); }
	[[nodiscard]] int	NumLeakPoints() const { return leakPoints.Num(); }
	[[nodiscard]] const idVec3 &	LeakPoint( int i ) const { return leakPoints[i]; }
	void				ShowLeak( bool show ) { showLeak = show; }
	[[nodiscard]] bool	LeakShown() const { return showLeak; }

	[[nodiscard]] idRenderWorld *	RenderWorld() const { return world; }
	// for editorWorldInfo: the primitives of all entities, the vertices of the preview's models,
	// and the preview models alive in the process
	void				CountPrimitives( int & brushes, int & patches, int & views, int & verts ) const;
	[[nodiscard]] static int	NumPreviewModels();

	// the game's rule: the spawn args are the entityDef's key/values with the map's on top
	static void			MergedSpawnArgs( const idDict & mapArgs, idDict & out );
	// A skinned model cannot be drawn without joints: the first frame of the class's idle animation,
	// or the bind pose. joints is Mem_Alloc16 memory the caller owns; an old block is freed.
	static void			PoseSkinnedModel( const idDict & args, renderEntity_t & renderEntity, idJointMat * & joints );
	void				Draw( const idVec3 & origin, const idMat3 & axis, float fovX, float fovY, int timeMsec );

private:
	// The render world of the document, with every entity shown; inside a level load.
	// compiled: try the .proc first; without one the geometry is the preview.
	void				BuildWorld( const char * fileName, bool compiled );
	void				FreeWorld();
	// the loose .proc was made from the geometry the document has
	[[nodiscard]] bool	ProcIsCurrent( const char * fileName ) const;
	void				ShowPreview();
	void				StartInteractions();
	// from entities[index].args: adds, updates or frees the model, the light and the views
	void				ShowEntity( int index );
	void				ShowPrimitives( int index );
	void				MakeView( int index, int primitive, editorPrimitiveView_t & view );
	void				BuildCells( int index );
	void				FreeCell( editorGeometryCell_t & cell );
	void				FreeViews( editorMapEntity_t & entity );
	void				UpdateViewBounds( editorMapEntity_t & entity );
	void				FreeEntity( editorMapEntity_t & entity );
	void				DrawMarkers( const idVec3 & viewOrigin );
	void				UpdateHeadLight( const idVec3 & viewOrigin );

	idStr				mapName;
	idMapFile			mapFile;
	idRenderWorld *		world = nullptr;
	idList<editorMapEntity_t>	entities;
	bool				previewGeometry = false;
	bool				unsaved = false;
	qhandle_t			headLightHandle = -1;
	renderLight_t		headLight;
	idList<idVec3>		leakPoints;
	bool				showLeak = true;
	// PrimitiveBounds(): by entity, nullptr = not asked for yet
	mutable idList<idList<idBounds> *>	primitiveBounds;
};
