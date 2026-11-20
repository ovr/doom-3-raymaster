// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include "d3xp/Game.h"
#include "renderer/Model.h"

#include "../api/EditorEngineApi.h"
#include "EditorPreview.h"
#include "EditorWorld.h"

namespace {

constexpr float		SPHERE_RADIUS = 32.0f;
constexpr int		SPHERE_RINGS = 24;
constexpr int		SPHERE_SEGMENTS = 48;
constexpr float		PREVIEW_FOV = 40.0f;
constexpr int		PREVIEW_TIME_MSEC = 1000;		// a fixed time: the same picture every time
constexpr int		MIN_PREVIEW_SIZE = 16;
constexpr float		BACKGROUND = 0.11f;

// A sphere with the texture wrapped twice around it, so a tiling material shows its seams.
[[nodiscard]] idRenderModel * MakeSphere() {
	idRenderModel * const model = renderModelManager->AllocModel();
	model->InitEmpty( "_editorPreviewSphere" );

	const int numVerts = ( SPHERE_RINGS + 1 ) * ( SPHERE_SEGMENTS + 1 );
	const int numIndexes = SPHERE_RINGS * SPHERE_SEGMENTS * 6;
	srfTriangles_t * const tri = model->AllocSurfaceTriangles( numVerts, numIndexes );

	for ( int ring = 0; ring <= SPHERE_RINGS; ring++ ) {
		const float v = static_cast<float>( ring ) / SPHERE_RINGS;
		const float pitch = ( v - 0.5f ) * idMath::PI;
		for ( int segment = 0; segment <= SPHERE_SEGMENTS; segment++ ) {
			const float u = static_cast<float>( segment ) / SPHERE_SEGMENTS;
			const float yaw = u * idMath::TWO_PI;
			const idVec3 normal( idMath::Cos( pitch ) * idMath::Cos( yaw ), idMath::Cos( pitch ) * idMath::Sin( yaw ), idMath::Sin( pitch ) );
			idDrawVert & vert = tri->verts[tri->numVerts++];
			vert.Clear();
			vert.xyz = normal * SPHERE_RADIUS;
			vert.SetNormal( normal );
			vert.SetTexCoord( u * 2.0f, 1.0f - v );
			vert.color[0] = vert.color[1] = vert.color[2] = vert.color[3] = 255;
		}
	}
	for ( int ring = 0; ring < SPHERE_RINGS; ring++ ) {
		for ( int segment = 0; segment < SPHERE_SEGMENTS; segment++ ) {
			const int a = ring * ( SPHERE_SEGMENTS + 1 ) + segment;
			const int b = a + SPHERE_SEGMENTS + 1;
			const int corners[6] = { a, b, a + 1, a + 1, b, b + 1 };
			for ( const int corner : corners ) {
				tri->indexes[tri->numIndexes++] = static_cast<triIndex_t>( corner );
			}
		}
	}

	modelSurface_t surface = {};
	surface.shader = declManager->FindMaterial( "_default" );
	surface.geometry = tri;
	model->AddSurface( surface );
	model->FinishSurfaces();
	return model;
}

void SetShaderParms( float * parms ) {
	parms[SHADERPARM_RED] = 1.0f;
	parms[SHADERPARM_GREEN] = 1.0f;
	parms[SHADERPARM_BLUE] = 1.0f;
	parms[SHADERPARM_ALPHA] = 1.0f;
}

}	// namespace

void idEditorPreview::Init() {
	world = renderSystem->AllocRenderWorld();
	// an empty world of one area; without it nothing added is ever drawn
	world->InitFromMap( nullptr );
	sphere = MakeSphere();

	// Show() puts them around the model
	const float brightness[2] = { 0.85f, 0.3f };
	for ( int i = 0; i < 2; i++ ) {
		// not = {}: idVec3 has an empty constructor of its own, so that would leave them unset
		renderLight_t & light = lights[i];
		memset( &light, 0, sizeof( light ) );
		light.axis = mat3_identity;
		light.pointLight = true;
		light.lightRadius.Set( 4096.0f, 4096.0f, 4096.0f );
		light.shader = declManager->FindMaterial( "lights/defaultPointLight" );
		light.noShadows = true;
		SetShaderParms( light.shaderParms );
		light.shaderParms[SHADERPARM_RED] = light.shaderParms[SHADERPARM_GREEN] = light.shaderParms[SHADERPARM_BLUE] = brightness[i];
		lightHandles[i] = world->AddLightDef( &light );
	}
	// the frontend needs the interaction table this makes
	world->GenerateAllInteractions();
}

void idEditorPreview::Shutdown() {
	queue.Clear();
	drawnSize = 0;
	if ( world != nullptr ) {
		renderSystem->FreeRenderWorld( world );
		world = nullptr;
	}
	if ( sphere != nullptr ) {
		renderModelManager->FreeModel( sphere );
		sphere = nullptr;
	}
	if ( joints != nullptr ) {
		Mem_Free16( joints );
		joints = nullptr;
	}
	entityHandle = -1;
	lightHandles[0] = lightHandles[1] = -1;
}

// Puts the job's model into the world and the camera where the whole model is in view.
void idEditorPreview::Show( const job_t & job, renderView_t & view ) {
	memset( &entity, 0, sizeof( entity ) );
	entity.axis = mat3_identity;
	SetShaderParms( entity.shaderParms );
	idVec3 viewDir( 1.0f, 0.0f, 0.0f );

	if ( job.kind == EDITOR_PREVIEW_MATERIAL ) {
		entity.hModel = sphere;
		entity.customShader = declManager->FindMaterial( job.name );
	} else {
		idDict args;
		if ( job.kind == EDITOR_PREVIEW_ENTITY_CLASS ) {
			idDict mapArgs;
			mapArgs.Set( "classname", job.name );
			idEditorWorld::MergedSpawnArgs( mapArgs, args );
			args.Delete( "origin" );
			args.Delete( "angle" );
			args.Delete( "rotation" );
		} else {
			args.Set( "model", job.name );
		}
		if ( args.GetString( "model" )[0] != '\0' ) {
			gameEdit->ParseSpawnArgsToRenderEntity( &args, &entity );
		}
		if ( entity.hModel == nullptr ) {
			entity.hModel = renderModelManager->DefaultModel();
		}
		if ( entity.hModel->NumJoints() > 0 ) {
			idEditorWorld::PoseSkinnedModel( args, entity, joints );
		} else {
			entity.bounds = entity.hModel->Bounds( &entity );
		}
		// models face +X: from the front, a little from the side and from above
		viewDir.Set( -1.0f, -0.55f, -0.35f );
		viewDir.Normalize();
	}
	if ( job.kind == EDITOR_PREVIEW_MATERIAL ) {
		entity.bounds = sphere->Bounds( &entity );
	}
	entity.origin.Zero();

	if ( entityHandle == -1 ) {
		entityHandle = world->AddEntityDef( &entity );
	} else {
		world->UpdateEntityDef( entityHandle, &entity );
	}

	const idVec3 center = entity.bounds.GetCenter();
	const float radius = Max( 1.0f, entity.bounds.GetRadius( center ) );
	const float distance = radius / idMath::Sin( DEG2RAD( PREVIEW_FOV * 0.5f ) ) * 1.05f;

	memset( &view, 0, sizeof( view ) );
	view.vieworg = center - viewDir * distance;
	// through angles: no roll, the horizon stays level
	view.viewaxis = viewDir.ToAngles().ToMat3();
	view.fov_x = PREVIEW_FOV;
	view.fov_y = PREVIEW_FOV;
	view.time[0] = PREVIEW_TIME_MSEC;
	view.time[1] = PREVIEW_TIME_MSEC;
	SetShaderParms( view.shaderParms );

	// a key light over the camera's left shoulder and a dim one from the right, below and behind
	const idVec3 & left = view.viewaxis[1];
	const idVec3 & up = view.viewaxis[2];
	lights[0].origin = view.vieworg + ( left + up ) * ( radius * 1.5f );
	lights[1].origin = center + ( viewDir * 0.5f - left * 2.5f - up ) * radius;
	for ( int i = 0; i < 2; i++ ) {
		world->UpdateLightDef( lightHandles[i], &lights[i] );
	}
}

bool idEditorPreview::Draw() {
	drawnSize = 0;
	if ( queue.Num() == 0 ) {
		return false;
	}
	if ( world == nullptr ) {
		Init();
	}
	const int size = Min( queue[0].size, Min( renderSystem->GetWidth(), renderSystem->GetHeight() ) );
	if ( size < MIN_PREVIEW_SIZE ) {
		return false;
	}

	renderView_t view;
	Show( queue[0], view );
	// debug lines belong to no world: without this the edit view's markers and gizmo are in the picture
	world->DebugClearLines( 0 );
	renderSystem->CropRenderSize( size, size );
	// a scene does not clear the colour, and the output still holds the frame before
	renderSystem->SetColor4( BACKGROUND, BACKGROUND, BACKGROUND, 1.0f );
	renderSystem->DrawStretchPic( 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 0.0f, 1.0f, 1.0f, declManager->FindMaterial( "_white" ) );
	renderSystem->SetColor4( 1.0f, 1.0f, 1.0f, 1.0f );
	world->RenderScene( &view );
	renderSystem->UnCrop();
	drawnSize = size;
	return true;
}

bool idEditorPreview::Finish( job_t & job, idList<byte> & rgb ) {
	if ( drawnSize == 0 || queue.Num() == 0 ) {
		return false;
	}
	const int size = drawnSize;
	drawnSize = 0;
	job = queue[0];
	job.size = size;
	queue.RemoveIndex( 0 );

	// the crop is the top left corner of the output; the rows arrive bottom first
	idList<byte> bottomFirst;
	bottomFirst.SetNum( size * size * 3 );
	renderSystem->ReadLastFrame( 0, renderSystem->GetHeight() - size, size, size, bottomFirst.Ptr(), true );
	rgb.SetNum( size * size * 3 );
	const int rowBytes = size * 3;
	for ( int y = 0; y < size; y++ ) {
		memcpy( rgb.Ptr() + y * rowBytes, bottomFirst.Ptr() + ( size - 1 - y ) * rowBytes, rowBytes );
	}
	return true;
}
