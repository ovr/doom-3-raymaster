// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

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

	The map file is the document: SetMapArgs() is the only way an entity changes, and Save() writes
	the file into fs_savepath, never into the game install.

	An entity keeps its index for as long as the map is open. A removed one stays in the list
	without key/values, so an undo can bring it back, and is not written.
*/

struct editorMapEntity_t {
	idDict				args;				// the map's own key/values over the entityDef's
	renderEntity_t		renderEntity;		// valid while modelHandle != -1
	renderLight_t		renderLight;		// valid while lightHandle != -1
	qhandle_t			modelHandle = -1;
	qhandle_t			lightHandle = -1;
	idJointMat *		joints = nullptr;	// the pose of a skinned model, Mem_Alloc16
	idBounds			bounds;				// around the origin, for the entities nothing draws
	idVec3				origin = vec3_origin;
	bool				isLight = false;
	bool				removed = false;
};

class idEditorWorld {
public:
						~idEditorWorld() { Close(); }

	// name as in a mapDef: "game/mc_underground"
	[[nodiscard]] bool	Open( const char * name );
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

	// what the entity covers in the world: its model, or its marker box
	[[nodiscard]] idBounds	WorldBounds( int index ) const;
	// the nearest entity on the ray, the world's own geometry hiding what is behind it; -1 = none
	[[nodiscard]] int	Pick( const idVec3 & start, const idVec3 & dir, float maxDistance ) const;

	// where a new view starts: the player start, or the middle of the entities
	void				GetStartView( idVec3 & origin, idAngles & angles ) const;

	[[nodiscard]] idRenderWorld *	RenderWorld() const { return world; }

	// the game's rule: the spawn args are the entityDef's key/values with the map's on top
	static void			MergedSpawnArgs( const idDict & mapArgs, idDict & out );
	// A skinned model cannot be drawn without joints: the first frame of the class's idle animation,
	// or the bind pose. joints is Mem_Alloc16 memory the caller owns; an old block is freed.
	static void			PoseSkinnedModel( const idDict & args, renderEntity_t & renderEntity, idJointMat * & joints );
	void				Draw( const idVec3 & origin, const idMat3 & axis, float fovX, float fovY, int timeMsec );

private:
	// the render world of the map file, with every entity shown; inside a level load
	[[nodiscard]] bool	BuildWorld( const char * fileName );
	// from entities[index].args: adds, updates or frees the model and the light
	void				ShowEntity( int index );
	void				FreeEntity( editorMapEntity_t & entity );
	void				DrawMarkers( const idVec3 & viewOrigin );

	idStr				mapName;
	idMapFile			mapFile;
	idRenderWorld *		world = nullptr;
	idList<editorMapEntity_t>	entities;
};
