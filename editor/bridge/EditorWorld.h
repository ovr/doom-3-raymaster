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
*/

struct editorMapEntity_t {
	idDict				args;				// the map's own key/values over the entityDef's
	qhandle_t			modelHandle = -1;
	qhandle_t			lightHandle = -1;
	idJointMat *		joints = nullptr;	// the pose of a skinned model, Mem_Alloc16
	idBounds			bounds;				// around the origin, for the entities nothing draws
	idVec3				origin = vec3_origin;
	bool				isLight = false;
};

class idEditorWorld {
public:
						~idEditorWorld() { Close(); }

	// name as in a mapDef: "game/mc_underground"
	[[nodiscard]] bool	Open( const char * name );
	void				Close();
	[[nodiscard]] bool	IsOpen() const { return world != nullptr; }
	[[nodiscard]] const char *	Name() const { return mapName.c_str(); }

	[[nodiscard]] int	NumEntities() const { return entities.Num(); }
	[[nodiscard]] const editorMapEntity_t &	Entity( int index ) const { return entities[index]; }
	[[nodiscard]] idMapFile &	MapFile() { return mapFile; }

	// where a new view starts: the player start, or the middle of the entities
	void				GetStartView( idVec3 & origin, idAngles & angles ) const;

	void				Draw( const idVec3 & origin, const idMat3 & axis, int timeMsec );

private:
	void				AddEntity( const idMapEntity & mapEntity );
	void				FreeEntity( editorMapEntity_t & entity );
	void				PoseSkinnedModel( editorMapEntity_t & entity, renderEntity_t & renderEntity ) const;
	void				DrawMarkers( const idVec3 & viewOrigin );

	idStr				mapName;
	idMapFile			mapFile;
	idRenderWorld *		world = nullptr;
	idList<editorMapEntity_t>	entities;
};
