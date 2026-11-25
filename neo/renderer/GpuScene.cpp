// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "../idlib/precompiled.h"

#include <algorithm>
#include <atomic>

#include "tr_local.h"
#include "Model_local.h"

idCVar r_rtStats( "r_rtStats", "0", CVAR_RENDERER | CVAR_BOOL, "prints the GPU scene of the ray traced passes once a second (and builds it)" );

// The scene whose last update the back end could not apply: its next Gather sends a full one.
static std::atomic<uint32> gpuSceneLost = 0;

/*
================================================================================================

idGpuScene: the front end

================================================================================================
*/

static uint32 gpuSceneNextId = 1;

idGpuScene::idGpuScene() : id( gpuSceneNextId++ ) {
}

void idGpuScene::Reset() {
	full = true;
}

void idGpuScene::FreeSlot( slot_t & slot ) {
	geometries.Free( slot.firstGeometry, slot.numGeometries );
	slot = slot_t();
}

void idGpuScene::FreeEntity( int index ) {
	if ( index >= 0 && index < slots.Num() ) {
		FreeSlot( slots[index] );
	}
}

// A surface the scene holds: static vertex cache geometry of a drawn, not translucent and not deformed material.
static bool R_GpuSceneSurface( const modelSurface_t & surf, const idMaterial * shader ) {
	const srfTriangles_t * tri = surf.geometry;
	if ( shader == nullptr || !shader->IsDrawn() || tri == nullptr || tri->numIndexes <= 0 || tri->numVerts <= 0 ) {
		return false;
	}
	if ( tri->ambientCache == 0 || tri->indexCache == 0 || !idVertexCache::CacheIsStatic( tri->ambientCache )
			|| !idVertexCache::CacheIsStatic( tri->indexCache ) ) {
		return false;
	}
	return shader->Coverage() != MC_TRANSLUCENT && shader->Deform() == DFRM_NONE && !shader->IsPortalSky();
}

// A stage that samples one plain texture with the surface's own texture coordinates.
static bool R_GpuScenePlainStage( const shaderStage_t & stage, const float * regs ) {
	return regs[stage.conditionRegister] != 0.0f && stage.texture.image != nullptr && stage.texture.cinematic == nullptr
		&& stage.texture.dynamic == DI_STATIC && stage.texture.texgen == TG_EXPLICIT && stage.newStage == nullptr;
}

// The depth pass, like the front end, draws nothing of a surface whose stages are all conditioned off (the wings
// of a dead wraith).
static bool R_GpuSceneStagesOff( const idMaterial & shader, const float * regs ) {
	for ( int i = 0; i < shader.GetNumStages(); i++ ) {
		if ( regs[shader.GetStage( i )->conditionRegister] != 0.0f ) {
			return false;
		}
	}
	return true;
}

// The record of one surface: the stages RB_RenderInteractions reads, an unlit stage that adds or replaces as the
// emissive and the alpha-tested one, with the depth pass's choice of the fast path.
static void R_GpuSceneMaterial( const idMaterial & shader, const float * regs, gpuSceneWrite_t & write ) {
	gpuSceneGeometry_t & record = write.record;
	const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	for ( int t = 0; t < GPU_SCENE_TEXTURES; t++ ) {
		record.matrices[t][0].Set( 1.0f, 0.0f, 0.0f, 0.0f );
		record.matrices[t][1].Set( 0.0f, 1.0f, 0.0f, 0.0f );
	}
	write.images[static_cast<int>( gpuSceneTexture_t::BUMP )] = globalImages->flatNormalMap;

	bool found[GPU_SCENE_TEXTURES] = {};
	if ( shader.GetFastPathBumpImage() != nullptr && !r_skipInteractionFastPath.GetBool() ) {
		write.images[static_cast<int>( gpuSceneTexture_t::BUMP )] = shader.GetFastPathBumpImage();
		write.images[static_cast<int>( gpuSceneTexture_t::DIFFUSE )] = shader.GetFastPathDiffuseImage();
		write.images[static_cast<int>( gpuSceneTexture_t::SPECULAR )] = shader.GetFastPathSpecularImage();
		record.colors[0].Set( 1.0f, 1.0f, 1.0f, 1.0f );
		record.colors[1] = ( shader.GetFastPathSpecularImage() != nullptr ) ? idVec4( 1.0f, 1.0f, 1.0f, 1.0f ) : vec4_zero;
		found[static_cast<int>( gpuSceneTexture_t::BUMP )] = true;
		found[static_cast<int>( gpuSceneTexture_t::DIFFUSE )] = true;
		found[static_cast<int>( gpuSceneTexture_t::SPECULAR )] = true;
	}

	const bool perforated = ( shader.Coverage() == MC_PERFORATED );
	for ( int i = 0; i < shader.GetNumStages(); i++ ) {
		const shaderStage_t & stage = *shader.GetStage( i );
		if ( regs[stage.conditionRegister] == 0.0f || stage.texture.image == nullptr ) {
			continue;
		}
		gpuSceneTexture_t texture = gpuSceneTexture_t::COUNT;
		idVec4 * color = nullptr;
		switch ( stage.lighting ) {
			case SL_BUMP:		texture = gpuSceneTexture_t::BUMP; break;
			case SL_DIFFUSE:	texture = gpuSceneTexture_t::DIFFUSE; color = &record.colors[0]; break;
			case SL_SPECULAR:	texture = gpuSceneTexture_t::SPECULAR; color = &record.colors[1]; break;
			case SL_AMBIENT: {
				const uint64 blend = stage.drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS );
				if ( R_GpuScenePlainStage( stage, regs ) && ( blend == ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO )
						|| blend == ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE ) ) ) {
					texture = gpuSceneTexture_t::EMISSIVE;
					color = &record.colors[2];
				}
				break;
			}
			default:
				break;
		}
		const int t = static_cast<int>( texture );
		if ( texture != gpuSceneTexture_t::COUNT && !found[t] ) {
			found[t] = true;
			write.images[t] = stage.texture.image;
			RB_SetupInteractionStage( &stage, regs, white, record.matrices[t], color != nullptr ? color->ToFloatPtr() : nullptr );
		}
		// the depth pass draws every enabled alpha-tested stage: a later one of the same image, matrix and colour alpha
		// lowers the reference (a monster's two burn stages), another image is left out
		const int alpha = static_cast<int>( gpuSceneTexture_t::ALPHA );
		if ( perforated && stage.hasAlphaTest ) {
			idVec4 matrix[2];
			RB_SetupInteractionStage( &stage, regs, white, matrix, nullptr );
			const float reference = regs[stage.alphaTestRegister];
			const float scale = regs[stage.color.registers[3]];
			if ( !found[alpha] ) {
				found[alpha] = true;
				write.images[alpha] = stage.texture.image;
				record.matrices[alpha][0] = matrix[0];
				record.matrices[alpha][1] = matrix[1];
				record.alphaTest.Set( reference, scale, 0.0f, 0.0f );
			} else if ( write.images[alpha] == stage.texture.image && matrix[0] == record.matrices[alpha][0]
					&& matrix[1] == record.matrices[alpha][1] && scale == record.alphaTest.y ) {
				record.alphaTest.x = Min( record.alphaTest.x, reference );
			}
		}
	}
	// without an enabled alpha-tested stage the depth pass draws the surface solid
	if ( perforated && !found[static_cast<int>( gpuSceneTexture_t::ALPHA )] ) {
		record.flags |= gpuSceneGeometry_t::GEO_OPAQUE;
	}
}

static uint64 R_GpuSceneHash( uint64 hash, uint32 value ) {
	for ( int i = 0; i < 4; i++ ) {
		hash ^= ( value >> ( i * 8 ) ) & 0xff;
		hash *= 0x100000001b3ull;
	}
	return hash;
}

void idGpuScene::BuildSlot( slot_t & slot, const idRenderEntityLocal & def, const idRenderModel & model, bool skinned ) {
	slot.model = &model;
	slot.modified = def.lastModifiedFrameNum;
	slot.skinned = skinned;
	slot.animated = false;
	slot.skinnedVerts = 0;

	// the records first: the registers decide which surfaces are drawn, so how many the slot holds this frame
	building.SetNum( 0 );
	uint64 key = 0xcbf29ce484222325ull;
	float scratch[MAX_EXPRESSION_REGISTERS];
	for ( int i = 0; i < model.NumSurfaces(); i++ ) {
		const modelSurface_t & surf = *model.Surface( i );
		const idMaterial * shader = R_RemapShaderBySkin( surf.shader, def.parms.customSkin, def.parms.customShader );
		if ( !R_GpuSceneSurface( surf, shader ) ) {
			continue;
		}
		const float * regs = shader->ConstantRegisters();
		if ( regs == nullptr ) {
			shader->EvaluateRegisters( scratch, def.parms.shaderParms, tr.viewDef->renderView.shaderParms,
				tr.viewDef->renderView.time[def.parms.timeGroup] * 0.001f, def.parms.referenceSound );
			regs = scratch;
			slot.animated = true;
		}
		if ( R_GpuSceneStagesOff( *shader, regs ) ) {
			continue;
		}
		const srfTriangles_t & tri = *surf.geometry;
		gpuSceneWrite_t & write = building.Alloc();
		memset( &write, 0, sizeof( write ) );
		write.material = shader;

		gpuSceneGeometry_t & record = write.record;
		bufferHandle_t buffer;
		int offset = 0;
		vertexCache.ResolveVertex( tri.ambientCache, buffer, offset );
		record.vertexOffset = static_cast<uint32>( offset );
		vertexCache.ResolveIndex( tri.indexCache, buffer, offset );
		record.indexOffset = static_cast<uint32>( offset );
		record.numIndexes = static_cast<uint32>( tri.numIndexes );
		record.numVerts = static_cast<uint32>( tri.numVerts );
		const bool subview = ( shader->GetSort() == static_cast<float>( SS_SUBVIEW ) );
		record.flags = ( shader->Coverage() != MC_PERFORATED ? gpuSceneGeometry_t::GEO_OPAQUE : 0 )
			| ( skinned ? gpuSceneGeometry_t::GEO_SKINNED : 0 )
			| ( shader->GetCullType() == CT_TWO_SIDED ? gpuSceneGeometry_t::GEO_TWO_SIDED : 0 )
			| ( ( shader->HasGui() || subview ) ? gpuSceneGeometry_t::GEO_NO_SHADING : 0 )
			| ( shader->SurfaceCastsShadow() ? gpuSceneGeometry_t::GEO_CASTS_SHADOW : 0 );
		record.skinnedOffset = static_cast<uint32>( slot.skinnedVerts * sizeof( idDrawVert ) );
		slot.skinnedVerts += skinned ? tri.numVerts : 0;
		R_GpuSceneMaterial( *shader, regs, write );

		key = R_GpuSceneHash( key, record.vertexOffset );
		key = R_GpuSceneHash( key, record.indexOffset );
		key = R_GpuSceneHash( key, record.numIndexes );
		key = R_GpuSceneHash( key, record.numVerts );
		key = R_GpuSceneHash( key, record.flags & gpuSceneGeometry_t::GEO_OPAQUE );
	}
	if ( building.Num() != slot.numGeometries ) {
		geometries.Free( slot.firstGeometry, slot.numGeometries );
		slot.firstGeometry = ( building.Num() > 0 ) ? geometries.Allocate( building.Num() ) : 0;
		slot.numGeometries = building.Num();
	}
	for ( int k = 0; k < building.Num(); k++ ) {
		building[k].index = slot.firstGeometry + k;
		Emit( building[k] );
	}
	slot.blasKey = skinned ? 0 : ( key | 1 );
}

void idGpuScene::Emit( const gpuSceneWrite_t & write ) {
	if ( write.index >= sent.Num() ) {
		const int first = sent.Num();
		sent.SetNum( geometries.Capacity() );
		for ( int i = first; i < sent.Num(); i++ ) {
			sent[i].valid = false;
		}
	}
	sent_t & held = sent[write.index];
	if ( held.valid && held.material == write.material && memcmp( held.images, write.images, sizeof( held.images ) ) == 0
			&& memcmp( &held.record, &write.record, sizeof( held.record ) ) == 0 ) {
		return;
	}
	held.valid = true;
	held.material = write.material;
	memcpy( held.images, write.images, sizeof( held.images ) );
	memcpy( &held.record, &write.record, sizeof( held.record ) );
	writes.Append( write );
}

// The joints a skinned model was drawn with this frame; nullptr when it was not drawn.
static const idRenderModelStatic * R_GpuSceneJoints( const idRenderModel & model ) {
	for ( int i = 0; i < model.NumSurfaces(); i++ ) {
		const srfTriangles_t * tri = model.Surface( i )->geometry;
		if ( tri != nullptr && tri->staticModelWithJoints != nullptr ) {
			const idRenderModelStatic * joints = tri->staticModelWithJoints;
			return vertexCache.CacheIsCurrent( joints->jointsInvertedBuffer ) ? joints : nullptr;
		}
	}
	return nullptr;
}

void idGpuScene::Gather( idRenderWorldLocal & world, viewDef_t & view ) {
	const int frame = tr.frameCount;
	if ( gatherFrame == frame && gathered != nullptr ) {
		view.gpuScene = gathered;
		return;
	}
	uint32 lost = id;
	if ( gpuSceneLost.compare_exchange_strong( lost, 0 ) || staticGeneration != vertexCache.StaticGeneration() ) {
		full = true;
	}
	staticGeneration = vertexCache.StaticGeneration();
	if ( full ) {
		slots.Clear();
		seenFrame.Clear();
		geometries.Clear();
		sent.Clear();
	}
	writes.SetNum( 0 );
	instances.SetNum( 0 );
	const int numDefs = world.entityDefs.Num();
	while ( slots.Num() < numDefs ) {
		slots.Append( slot_t() );
		seenFrame.Append( -1 );
	}

	const bool gpuSkinning = r_useGPUSkinning.GetBool();
	int numGeometries = 0;
	for ( int area = 0; area < world.numPortalAreas; area++ ) {
		if ( view.connectedAreas != nullptr && !view.connectedAreas[area] ) {
			continue;
		}
		const portalArea_t & portalArea = world.portalAreas[area];
		for ( const areaReference_t * ref = portalArea.entityRefs.areaNext; ref != &portalArea.entityRefs; ref = ref->areaNext ) {
			const idRenderEntityLocal & def = *ref->entity;
			const int index = def.index;
			if ( seenFrame[index] == frame ) {
				continue;
			}
			seenFrame[index] = frame;

			// as the view would draw it: not the weapon, nothing only another view sees, no xray
			const renderEntity_t & parms = def.parms;
			if ( parms.hModel == nullptr || parms.weaponDepthHack || parms.xrayIndex == 2
					|| ( parms.suppressSurfaceInViewID != 0 && parms.suppressSurfaceInViewID == view.renderView.viewID )
					|| ( parms.allowSurfaceInViewID != 0 && parms.allowSurfaceInViewID != view.renderView.viewID ) ) {
				continue;
			}
			const idRenderModel * model = nullptr;
			const idRenderModelStatic * joints = nullptr;
			if ( parms.hModel->IsDynamicModel() == DM_STATIC ) {
				model = parms.hModel;
			} else if ( gpuSkinning && def.dynamicModel != nullptr ) {
				// a skinned model drawn this frame: its bind pose is static, its joints uploaded. A cached one counts too
				// (a monster at rest is not instantiated again), one not drawn has no current joints
				joints = R_GpuSceneJoints( *def.dynamicModel );
				model = ( joints != nullptr ) ? def.dynamicModel : nullptr;
			}
			if ( model == nullptr ) {
				continue;
			}
			slot_t & slot = slots[index];
			const bool skinned = ( joints != nullptr );
			if ( slot.model != model || slot.modified != def.lastModifiedFrameNum || slot.animated || slot.skinned != skinned ) {
				BuildSlot( slot, def, *model, skinned );
			}
			if ( slot.numGeometries == 0 ) {
				continue;
			}
			numGeometries += slot.numGeometries;

			gpuSceneInstance_t & instance = instances.Alloc();
			memset( &instance, 0, sizeof( instance ) );
			for ( int r = 0; r < 3; r++ ) {
				for ( int c = 0; c < 4; c++ ) {
					instance.transform[r][c] = def.modelMatrix[c * 4 + r];
				}
			}
			instance.firstGeometry = static_cast<uint32>( slot.firstGeometry );
			instance.numGeometries = static_cast<uint32>( slot.numGeometries );
			instance.mask = gpuSceneInstance_t::MASK_VISIBLE | ( parms.noShadow ? 0 : gpuSceneInstance_t::MASK_SHADOW );
			instance.skinnedVerts = static_cast<uint32>( slot.skinnedVerts );
			instance.blasKey = slot.blasKey;
			if ( skinned ) {
				instance.jointCache = joints->jointsInvertedBuffer;
				instance.numJoints = joints->numInvertedJoints;
			}
		}
	}

	std::sort( writes.Ptr(), writes.Ptr() + writes.Num(), []( const gpuSceneWrite_t & a, const gpuSceneWrite_t & b ) { return a.index < b.index; } );
	gpuSceneFrame_t * out = static_cast<gpuSceneFrame_t *>( R_ClearedFrameAlloc( sizeof( *out ), FRAME_ALLOC_GPU_SCENE ) );
	out->sceneId = id;
	out->sequence = ++sequence;
	out->full = full;
	out->staticGeneration = staticGeneration;
	out->geometryCapacity = geometries.Capacity();
	out->numWrites = writes.Num();
	out->writes = static_cast<gpuSceneWrite_t *>( R_FrameAlloc( Max( 1, writes.Num() ) * sizeof( gpuSceneWrite_t ), FRAME_ALLOC_GPU_SCENE ) );
	memcpy( out->writes, writes.Ptr(), writes.Num() * sizeof( gpuSceneWrite_t ) );
	out->numInstances = instances.Num();
	out->instances = static_cast<gpuSceneInstance_t *>( R_FrameAlloc( Max( 1, instances.Num() ) * sizeof( gpuSceneInstance_t ), FRAME_ALLOC_GPU_SCENE ) );
	memcpy( out->instances, instances.Ptr(), instances.Num() * sizeof( gpuSceneInstance_t ) );
	out->numGeometries = numGeometries;

	full = false;
	gatherFrame = frame;
	gathered = out;
	view.gpuScene = out;
}

/*
================================================================================================

The back end

================================================================================================
*/

void RB_UpdateGpuScene( gpuSceneFrame_t & frame ) {
	for ( int i = 0; i < frame.numWrites; i++ ) {
		gpuSceneWrite_t & write = frame.writes[i];
		const_cast<idMaterial *>( write.material )->EnsureNotPurged();
		for ( int t = 0; t < GPU_SCENE_TEXTURES; t++ ) {
			write.textures[t] = ( write.images[t] != nullptr ) ? write.images[t]->Handle() : textureHandle_t::NONE;
		}
	}
	for ( int i = 0; i < frame.numInstances; i++ ) {
		gpuSceneInstance_t & instance = frame.instances[i];
		if ( instance.jointCache != 0 ) {
			int numJoints = 0;
			vertexCache.ResolveJoint( instance.jointCache, instance.jointBuffer, instance.jointOffset, numJoints );
		}
	}
	const gpuSceneUpdate_t update = { &frame, vertexCache.StaticVertexBuffer(), vertexCache.StaticIndexBuffer() };
	gpuSceneStats_t stats;
	const bool applied = renderBackend->UpdateGpuScene( update, stats );
	static int refused = 0;
	if ( !applied ) {
		gpuSceneLost.store( frame.sceneId );
		refused++;
	}

	static int lastPrint = 0;
	if ( r_rtStats.GetBool() && Sys_Milliseconds() - lastPrint >= 1000 ) {
		lastPrint = Sys_Milliseconds();
		common->Printf( "rt scene %u.%u: %d instances, %d geometries, %d written%s, capacity %d, %d refused; tlas %d (%d waiting), "
			"blas %d (%d built, %d compacted, %.1f MB), skinned %d (%d verts), %.1f MB per frame\n", frame.sceneId, frame.sequence,
			frame.numInstances, frame.numGeometries, frame.numWrites, frame.full ? " (full)" : "", frame.geometryCapacity, refused,
			stats.tlasInstances, stats.waiting, stats.blas, stats.built, stats.compacted, stats.blasBytes / ( 1024.0 * 1024.0 ), stats.skinned,
			stats.skinnedVerts, stats.frameBytes / ( 1024.0 * 1024.0 ) );
		refused = 0;
	}
}
