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
constexpr float		MARKER_DISTANCE = 1500.0f;
constexpr float		DEFAULT_FOV_X = 90.0f;
constexpr float		PLAYER_EYE_HEIGHT = 68.0f;
const idBounds		DEFAULT_MARKER_BOUNDS( idVec3( -8.0f, -8.0f, -8.0f ), idVec3( 8.0f, 8.0f, 8.0f ) );

// the game's rule: the spawn args are the entityDef's key/values with the map's on top
void MergedSpawnArgs( const idDict & mapArgs, idDict & out ) {
	out = mapArgs;
	const char * const classname = mapArgs.GetString( "classname" );
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
		entities.SetNum( 0 );
		entities.SetGranularity( 1024 );
		for ( int i = 0; i < mapFile.GetNumEntities(); i++ ) {
			AddEntity( *mapFile.GetEntity( i ) );
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

void idEditorWorld::AddEntity( const idMapEntity & mapEntity ) {
	editorMapEntity_t & entity = entities.Alloc();
	MergedSpawnArgs( mapEntity.epairs, entity.args );
	entity.origin = entity.args.GetVector( "origin", "0 0 0" );
	entity.bounds = MarkerBounds( entity.args );

	// the worldspawn's geometry is the world's areas, which InitFromMap already made
	if ( entities.Num() == 1 ) {
		return;
	}

	entity.isLight = ( idStr::Icmp( entity.args.GetString( "spawnclass" ), "idLight" ) == 0 );
	if ( entity.isLight ) {
		renderLight_t renderLight;
		gameEdit->ParseSpawnArgsToRenderLight( &entity.args, &renderLight );
		entity.lightHandle = world->AddLightDef( &renderLight );
	}

	if ( entity.args.GetString( "model" )[0] == '\0' ) {
		return;
	}
	renderEntity_t renderEntity;
	gameEdit->ParseSpawnArgsToRenderEntity( &entity.args, &renderEntity );
	if ( renderEntity.hModel == nullptr || renderEntity.hModel->IsDefaultModel() ) {
		return;
	}
	if ( renderEntity.hModel->NumJoints() > 0 ) {
		PoseSkinnedModel( entity, renderEntity );
	}
	entity.modelHandle = world->AddEntityDef( &renderEntity );
}

// A skinned model cannot be drawn without joints: the first frame of its idle animation, or the
// bind pose when the class has none.
void idEditorWorld::PoseSkinnedModel( editorMapEntity_t & entity, renderEntity_t & renderEntity ) const {
	const idRenderModel * const model = renderEntity.hModel;
	const int numJoints = model->NumJoints();
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
		all.AddPoint( entities[i].origin );
	}
	if ( !all.IsCleared() ) {
		origin = all.GetCenter();
	}
}

void idEditorWorld::DrawMarkers( const idVec3 & viewOrigin ) {
	const float maxDistanceSqr = Square( MARKER_DISTANCE );
	for ( int i = 1; i < entities.Num(); i++ ) {
		const editorMapEntity_t & entity = entities[i];
		if ( entity.modelHandle != -1 || ( entity.origin - viewOrigin ).LengthSqr() > maxDistanceSqr ) {
			continue;
		}
		idVec3 color = entity.isLight ? entity.args.GetVector( "_color", "1 1 1" ) : entity.args.GetVector( "editor_color", "0.3 0.6 1" );
		world->DebugBounds( idVec4( color.x, color.y, color.z, 1.0f ), entity.bounds, entity.origin );
	}
}

void idEditorWorld::Draw( const idVec3 & origin, const idMat3 & axis, int timeMsec ) {
	if ( world == nullptr ) {
		return;
	}
	DrawMarkers( origin );

	renderView_t view = {};
	view.vieworg = origin;
	view.viewaxis = axis;
	view.fov_x = DEFAULT_FOV_X;
	// the vertical angle that keeps pixels square for this window
	const float aspect = static_cast<float>( renderSystem->GetWidth() ) / static_cast<float>( Max( 1, renderSystem->GetHeight() ) );
	view.fov_y = RAD2DEG( 2.0f * idMath::ATan( idMath::Tan( DEG2RAD( view.fov_x ) * 0.5f ) / aspect ) );
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
