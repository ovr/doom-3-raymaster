// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include "d3xp/Game.h"
#include "renderer/Model.h"

#include "EditorWorld.h"

namespace {

// entities without a model are drawn as a box when the view is this near
constexpr float		MARKER_DISTANCE = 512.0f;
constexpr float		PLAYER_EYE_HEIGHT = 68.0f;
const idBounds		DEFAULT_MARKER_BOUNDS( idVec3( -8.0f, -8.0f, -8.0f ), idVec3( 8.0f, 8.0f, 8.0f ) );

// the game's rule: the spawn args are the entityDef's key/values with the map's on top
void MergedSpawnArgs( const idDict & mapArgs, idDict & out ) {
	out = mapArgs;
	const char * const classname = mapArgs.GetString( "classname" );
	if ( classname[0] == '\0' ) {
		return;
	}
	if ( const idDict * const defaults = gameEdit->FindEntityDefDict( classname, false ) ) {
		out.SetDefaults( defaults );
	}
}

[[nodiscard]] idBounds MarkerBounds( const idDict & args ) {
	idBounds bounds;
	if ( args.GetVector( "editor_mins", "", bounds[0] ) && args.GetVector( "editor_maxs", "", bounds[1] ) ) {
		return bounds;
	}
	if ( args.GetVector( "mins", "", bounds[0] ) && args.GetVector( "maxs", "", bounds[1] ) ) {
		return bounds;
	}
	return DEFAULT_MARKER_BOUNDS;
}

}	// namespace

bool idEditorWorld::Open( const char * name ) {
	Close();

	idStr cleanName = name;
	cleanName.StripFileExtension();
	cleanName.BackSlashesToSlashes();
	const idStr fileName = idStr( "maps/" ) + cleanName + ".map";

	const int start = Sys_Milliseconds();
	common->Printf( "--------- Editor: open %s ---------\n", cleanName.c_str() );

	// the map's own resource file holds its .map, its .bproc and the media only it uses
	fileSystem->BeginLevelLoad( cleanName, nullptr, 0 );
	renderSystem->BeginLevelLoad();
	declManager->BeginLevelLoad();

	bool ok = mapFile.Parse( fileName, true, false, true );
	if ( !ok ) {
		common->Warning( "Editor: couldn't read %s", fileName.c_str() );
	} else {
		world = renderSystem->AllocRenderWorld();
		ok = world->InitFromMap( fileName );
		if ( !ok ) {
			common->Warning( "Editor: %s has no compiled geometry (.proc)", fileName.c_str() );
		}
	}

	if ( ok ) {
		mapName = cleanName;
		entities.SetGranularity( 1024 );
		entities.SetNum( mapFile.GetNumEntities() );
		for ( int i = 0; i < entities.Num(); i++ ) {
			MergedSpawnArgs( mapFile.GetEntity( i )->epairs, entities[i].args );
			ShowEntity( i );
		}
		// as the game does after spawning; the frontend also needs the interaction table this makes
		world->GenerateAllInteractions();
	}

	declManager->EndLevelLoad();
	renderSystem->EndLevelLoad();
	fileSystem->EndLevelLoad();

	if ( !ok ) {
		Close();
		fileSystem->UnloadMapResources( cleanName );
		return false;
	}
	common->Printf( "%d entities, %d msec to open %s\n", entities.Num(), Sys_Milliseconds() - start, cleanName.c_str() );
	return true;
}

void idEditorWorld::Close() {
	for ( int i = 0; i < entities.Num(); i++ ) {
		FreeEntity( entities[i] );
	}
	entities.Clear();
	if ( world != nullptr ) {
		renderSystem->FreeRenderWorld( world );
		world = nullptr;
	}
	mapFile.RemoveAllEntities();
	if ( !mapName.IsEmpty() ) {
		fileSystem->UnloadMapResources( mapName );
		mapName.Clear();
	}
}

bool idEditorWorld::Save() {
	if ( !IsOpen() ) {
		return false;
	}
	return mapFile.Write( idStr( "maps/" ) + mapName, ".map", "fs_savepath" );
}

void idEditorWorld::SetMapArgs( int index, const idDict & mapArgs ) {
	mapFile.GetEntity( index )->epairs = mapArgs;
	MergedSpawnArgs( mapArgs, entities[index].args );
	ShowEntity( index );
}

int idEditorWorld::AddEntity() {
	const int index = mapFile.AddEntity( new ( TAG_IDLIB ) idMapEntity );
	entities.Alloc().removed = true;
	assert( entities.Num() == mapFile.GetNumEntities() );
	return index;
}

int idEditorWorld::FindEntity( const char * name ) const {
	for ( int i = 1; i < entities.Num(); i++ ) {
		if ( !entities[i].removed && idStr::Icmp( entities[i].args.GetString( "name" ), name ) == 0 ) {
			return i;
		}
	}
	return -1;
}

idStr idEditorWorld::UniqueName( const char * base ) const {
	for ( int n = 1; ; n++ ) {
		const idStr name = va( "%s_%d", base, n );
		if ( FindEntity( name ) == -1 ) {
			return name;
		}
	}
}

void idEditorWorld::ShowEntity( int index ) {
	editorMapEntity_t & entity = entities[index];
	entity.removed = ( index != 0 && entity.args.GetNumKeyVals() == 0 );
	if ( entity.removed ) {
		FreeEntity( entity );
		entity.isLight = false;
		return;
	}
	entity.origin = entity.args.GetVector( "origin", "0 0 0" );
	entity.bounds = MarkerBounds( entity.args );

	// the worldspawn's geometry is the world's areas, which InitFromMap already made
	if ( index == 0 ) {
		return;
	}

	entity.isLight = ( idStr::Icmp( entity.args.GetString( "spawnclass" ), "idLight" ) == 0 );
	if ( entity.isLight ) {
		gameEdit->ParseSpawnArgsToRenderLight( &entity.args, &entity.renderLight );
		if ( entity.lightHandle == -1 ) {
			entity.lightHandle = world->AddLightDef( &entity.renderLight );
		} else {
			world->UpdateLightDef( entity.lightHandle, &entity.renderLight );
		}
	} else if ( entity.lightHandle != -1 ) {
		world->FreeLightDef( entity.lightHandle );
		entity.lightHandle = -1;
	}

	bool hasModel = false;
	if ( entity.args.GetString( "model" )[0] != '\0' ) {
		gameEdit->ParseSpawnArgsToRenderEntity( &entity.args, &entity.renderEntity );
		hasModel = ( entity.renderEntity.hModel != nullptr && !entity.renderEntity.hModel->IsDefaultModel() );
	}
	if ( !hasModel ) {
		if ( entity.modelHandle != -1 ) {
			world->FreeEntityDef( entity.modelHandle );
			entity.modelHandle = -1;
		}
		return;
	}

	// Pick() finds the map entity of a hit model by this
	entity.renderEntity.entityNum = index;
	if ( entity.renderEntity.hModel->NumJoints() > 0 ) {
		PoseSkinnedModel( entity );
	}
	if ( entity.modelHandle == -1 ) {
		entity.modelHandle = world->AddEntityDef( &entity.renderEntity );
	} else {
		world->UpdateEntityDef( entity.modelHandle, &entity.renderEntity );
	}
}

// A skinned model cannot be drawn without joints: the first frame of its idle animation, or the
// bind pose when the class has none.
void idEditorWorld::PoseSkinnedModel( editorMapEntity_t & entity ) const {
	renderEntity_t & renderEntity = entity.renderEntity;
	const idRenderModel * const model = renderEntity.hModel;
	const int numJoints = model->NumJoints();
	if ( entity.joints != nullptr ) {
		Mem_Free16( entity.joints );
	}
	entity.joints = static_cast<idJointMat *>( Mem_Alloc16( SIMD_ROUND_JOINTS( numJoints ) * sizeof( idJointMat ), TAG_JOINTMAT ) );

	const char * const classname = entity.args.GetString( "classname" );
	const idMD5Anim * anim = gameEdit->ANIM_GetAnimFromEntityDef( classname, entity.args.GetString( "anim", "idle" ) );
	if ( anim != nullptr && gameEdit->ANIM_GetNumFrames( anim ) > 0 ) {
		gameEdit->ANIM_CreateAnimFrame( model, anim, numJoints, entity.joints, 1, vec3_origin, false );
	} else {
		SIMDProcessor->ConvertJointQuatsToJointMats( entity.joints, model->GetDefaultPose(), numJoints );
		const idMD5Joint * const md5Joints = model->GetJoints();
		for ( int i = 1; i < numJoints; i++ ) {
			if ( md5Joints[i].parent != nullptr ) {
				entity.joints[i] *= entity.joints[static_cast<int>( md5Joints[i].parent - md5Joints )];
			}
		}
	}
	SIMD_INIT_LAST_JOINT( entity.joints, numJoints );

	renderEntity.joints = entity.joints;
	renderEntity.numJoints = numJoints;
	renderEntity.bounds = model->Bounds( &renderEntity );
}

void idEditorWorld::FreeEntity( editorMapEntity_t & entity ) {
	if ( world != nullptr ) {
		if ( entity.modelHandle != -1 ) {
			world->FreeEntityDef( entity.modelHandle );
		}
		if ( entity.lightHandle != -1 ) {
			world->FreeLightDef( entity.lightHandle );
		}
	}
	entity.modelHandle = -1;
	entity.lightHandle = -1;
	if ( entity.joints != nullptr ) {
		Mem_Free16( entity.joints );
		entity.joints = nullptr;
	}
}

idBounds idEditorWorld::WorldBounds( int index ) const {
	const editorMapEntity_t & entity = entities[index];
	idBounds bounds;
	if ( entity.modelHandle != -1 ) {
		bounds.FromTransformedBounds( entity.renderEntity.bounds, entity.renderEntity.origin, entity.renderEntity.axis );
	} else {
		bounds = entity.bounds.Translate( entity.origin );
	}
	return bounds;
}

int idEditorWorld::Pick( const idVec3 & start, const idVec3 & dir, float maxDistance ) const {
	if ( world == nullptr ) {
		return -1;
	}
	int picked = -1;
	float distance = maxDistance;

	// models, and the world's geometry as entity 0
	modelTrace_t trace = {};
	if ( world->Trace( trace, start, start + dir * maxDistance, 0.0f, false ) && trace.entity != nullptr ) {
		distance = trace.fraction * maxDistance;
		const int index = trace.entity->entityNum;
		picked = ( index > 0 && index < entities.Num() ) ? index : -1;
	}

	// the boxes of what has no model
	for ( int i = 1; i < entities.Num(); i++ ) {
		const editorMapEntity_t & entity = entities[i];
		if ( entity.modelHandle != -1 || entity.removed ) {
			continue;
		}
		float scale = 0.0f;
		if ( entity.bounds.Translate( entity.origin ).RayIntersection( start, dir, scale ) && scale >= 0.0f && scale < distance ) {
			distance = scale;
			picked = i;
		}
	}
	return picked;
}

void idEditorWorld::GetStartView( idVec3 & origin, idAngles & angles ) const {
	angles.Zero();
	origin.Zero();

	for ( int i = 0; i < entities.Num(); i++ ) {
		const editorMapEntity_t & entity = entities[i];
		if ( idStr::Icmp( entity.args.GetString( "classname" ), "info_player_start" ) == 0 ) {
			origin = entity.origin + idVec3( 0.0f, 0.0f, PLAYER_EYE_HEIGHT );
			angles.yaw = entity.args.GetFloat( "angle" );
			return;
		}
	}

	idBounds all;
	all.Clear();
	for ( int i = 1; i < entities.Num(); i++ ) {
		if ( !entities[i].removed ) {
			all.AddPoint( entities[i].origin );
		}
	}
	if ( !all.IsCleared() ) {
		origin = all.GetCenter();
	}
}

void idEditorWorld::DrawMarkers( const idVec3 & viewOrigin ) {
	const float maxDistanceSqr = Square( MARKER_DISTANCE );
	for ( int i = 1; i < entities.Num(); i++ ) {
		const editorMapEntity_t & entity = entities[i];
		if ( entity.modelHandle != -1 || entity.removed || ( entity.origin - viewOrigin ).LengthSqr() > maxDistanceSqr ) {
			continue;
		}
		idVec3 color = entity.isLight ? entity.args.GetVector( "_color", "1 1 1" ) : entity.args.GetVector( "editor_color", "0.3 0.6 1" );
		world->DebugBounds( idVec4( color.x, color.y, color.z, 1.0f ), entity.bounds, entity.origin );
	}
}

void idEditorWorld::Draw( const idVec3 & origin, const idMat3 & axis, float fovX, float fovY, int timeMsec ) {
	if ( world == nullptr ) {
		return;
	}
	DrawMarkers( origin );

	renderView_t view = {};
	view.vieworg = origin;
	view.viewaxis = axis;
	view.fov_x = fovX;
	view.fov_y = fovY;
	view.time[0] = timeMsec;
	view.time[1] = timeMsec;
	for ( float & parm : view.shaderParms ) {
		parm = 1.0f;
	}
	view.shaderParms[4] = 0.0f;
	view.shaderParms[5] = 0.0f;
	view.shaderParms[6] = 0.0f;
	view.shaderParms[7] = 0.0f;
	world->RenderScene( &view );
}
