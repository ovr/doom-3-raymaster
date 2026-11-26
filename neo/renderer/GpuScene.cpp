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

void idGpuScene::BuildSlot( slot_t & slot, const idRenderEntityLocal & def, const idRenderModel & model, bool skinned ) {
	slot.model = &model;
	slot.modified = def.lastModifiedFrameNum;
	slot.skinned = skinned;
	slot.animated = false;

	// the records first: the registers decide which surfaces are drawn, so how many the slot holds this frame
	building.SetNum( 0 );
	float scratch[MAX_EXPRESSION_REGISTERS];
	float generated[MAX_ENTITY_SHADER_PARMS];
	const float * shaderParms = R_EntityShaderParms( def.parms, *tr.viewDef, generated );
	for ( int i = 0; i < model.NumSurfaces(); i++ ) {
		const modelSurface_t & surf = *model.Surface( i );
		const idMaterial * shader = R_RemapShaderBySkin( surf.shader, def.parms.customSkin, def.parms.customShader );
		if ( !R_GpuSceneSurface( surf, shader ) ) {
			continue;
		}
		const float * regs = shader->ConstantRegisters();
		if ( regs == nullptr ) {
			shader->EvaluateRegisters( scratch, shaderParms, tr.viewDef->renderView.shaderParms,
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
			| ( shader->SurfaceCastsShadow() ? gpuSceneGeometry_t::GEO_CASTS_SHADOW : 0 )
			| ( shader->TestMaterialFlag( MF_NOSELFSHADOW ) ? gpuSceneGeometry_t::GEO_NO_SELF_SHADOW : 0 );
		R_GpuSceneMaterial( *shader, regs, write );
	}
	// an instance culls and casts as a whole: each group of the records is one
	const std::span<gpuSceneWrite_t> records( building.Ptr(), static_cast<size_t>( building.Num() ) );
	slot.groups = GpuScene_OrderByGroup( records );
	for ( size_t group = 0, first = 0; group < GPU_SCENE_GROUPS; group++ ) {
		const size_t count = static_cast<size_t>( slot.groups.count[group] );
		slot.blasKey[group] = skinned ? 0 : GpuScene_BlasKey( records.subspan( first, count ) );
		first += count;
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

// The lights of the gathered areas: R_AddSingleLight's suppression and registers, not its culling against the view.
void idGpuScene::GatherLights( idRenderWorldLocal & world, const viewDef_t & view ) {
	lights.SetNum( 0 );
	while ( lightSeen.Num() < world.lightDefs.Num() ) {
		lightSeen.Append( -1 );
	}
	const float lightScale = r_lightScale.GetFloat();
	float scratch[MAX_EXPRESSION_REGISTERS];
	for ( int area = 0; area < world.numPortalAreas; area++ ) {
		if ( view.connectedAreas != nullptr && !view.connectedAreas[area] ) {
			continue;
		}
		const portalArea_t & portalArea = world.portalAreas[area];
		for ( const areaReference_t * ref = portalArea.lightRefs.areaNext; ref != &portalArea.lightRefs; ref = ref->areaNext ) {
			const idRenderLightLocal & light = *ref->light;
			if ( lightSeen[light.index] == gatherCount ) {
				continue;
			}
			lightSeen[light.index] = gatherCount;
			if ( !GpuScene_LightReaches( light.areaNum, view.connectedAreas ) ) {
				continue;
			}
			const idMaterial * shader = light.lightShader;
			const renderLight_t & parms = light.parms;
			if ( shader == nullptr || shader->IsFogLight() || shader->IsBlendLight() || light.falloffImage == nullptr ) {
				continue;
			}
			if ( !r_skipSuppress.GetBool() && ( ( parms.suppressLightInViewID != 0 && parms.suppressLightInViewID == view.renderView.viewID )
					|| ( parms.allowLightInViewID != 0 && parms.allowLightInViewID != view.renderView.viewID ) ) ) {
				continue;
			}
			const float * regs = shader->ConstantRegisters();
			if ( regs == nullptr ) {
				shader->EvaluateRegisters( scratch, parms.shaderParms, view.renderView.shaderParms, view.renderView.time[0] * 0.001f,
					parms.referenceSound );
				regs = scratch;
			}
			const bool ambient = shader->IsAmbientLight();
			for ( int s = 0; s < shader->GetNumStages(); s++ ) {
				const shaderStage_t & stage = *shader->GetStage( s );
				if ( regs[stage.conditionRegister] == 0.0f || stage.texture.image == nullptr ) {
					continue;
				}
				// R_AddSingleLight snaps tiny values to zero
				idVec3 color;
				for ( int c = 0; c < 3; c++ ) {
					const float value = regs[stage.color.registers[c]];
					color[c] = ( value < 0.001f ) ? 0.0f : value;
				}
				if ( color == vec3_zero ) {
					continue;
				}
				gpuSceneLightWrite_t & write = lights.Alloc();
				memset( &write, 0, sizeof( write ) );
				gpuSceneLight_t & out = write.light;
				out.origin.Set( light.globalLightOrigin.x, light.globalLightOrigin.y, light.globalLightOrigin.z, 1.0f );
				idPlane project[4];
				for ( int i = 0; i < 4; i++ ) {
					project[i] = light.lightProject[i];
				}
				if ( stage.texture.hasMatrix ) {
					float matrix[16];
					RB_GetShaderTextureMatrix( regs, &stage.texture, matrix );
					RB_BakeTextureMatrixIntoTexgen( project, matrix );
				}
				for ( int i = 0; i < 4; i++ ) {
					out.project[i].Set( project[i][0], project[i][1], project[i][2], project[i][3] );
				}
				out.color.Set( lightScale * color.x, lightScale * color.y, lightScale * color.z, regs[stage.color.registers[3]] );
				const idBounds & bounds = light.globalLightBounds;
				out.boundsMin.Set( bounds[0].x, bounds[0].y, bounds[0].z, 0.0f );
				out.boundsMax.Set( bounds[1].x, bounds[1].y, bounds[1].z, 0.0f );
				out.flags = ambient ? gpuSceneLight_t::LIGHT_AMBIENT : ( light.LightCastsShadows() ? gpuSceneLight_t::LIGHT_SHADOWS : 0 );
				out.emitterRadius = R_LightEmitterRadius( light, view.rayTracing.lightSize );
				write.material = shader;
				write.images[0] = stage.texture.image;
				write.images[1] = light.falloffImage;
			}
		}
	}
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

// Whether two views of a frame gather the same update: everything Gather reads of a view.
static bool R_GpuSceneSameView( const viewDef_t & a, const viewDef_t & b, int numAreas ) {
	const renderView_t & viewA = a.renderView;
	const renderView_t & viewB = b.renderView;
	if ( viewA.viewID != viewB.viewID || memcmp( viewA.time, viewB.time, sizeof( viewA.time ) ) != 0
			|| memcmp( viewA.shaderParms, viewB.shaderParms, sizeof( viewA.shaderParms ) ) != 0 ) {
		return false;
	}
	if ( a.connectedAreas == nullptr || b.connectedAreas == nullptr ) {
		return a.connectedAreas == b.connectedAreas;
	}
	return memcmp( a.connectedAreas, b.connectedAreas, numAreas * sizeof( bool ) ) == 0;
}

void idGpuScene::Gather( idRenderWorldLocal & world, viewDef_t & view ) {
	if ( gatherFrame != tr.frameCount ) {
		gatherFrame = tr.frameCount;
		gathers.SetNum( 0 );
		writes.SetNum( 0 );
		frameBase = sequence;
		frameFull = false;
	}
	for ( int i = 0; i < gathers.Num(); i++ ) {
		if ( R_GpuSceneSameView( *gathers[i].view, view, world.numPortalAreas ) ) {
			view.gpuScene = gathers[i].out;
			return;
		}
	}
	uint32 lost = id;
	if ( gpuSceneLost.compare_exchange_strong( lost, 0 ) || staticGeneration != vertexCache.StaticGeneration() ) {
		full = true;
	}
	staticGeneration = vertexCache.StaticGeneration();
	if ( full ) {
		slots.Clear();
		seen.Clear();
		geometries.Clear();
		sent.Clear();
		writes.SetNum( 0 );
		frameFull = true;
		full = false;
	}
	// another view of the frame (a portal sky's before the player's) keeps the frame's writes and adds its own
	gatherCount++;
	instances.SetNum( 0 );
	const int numDefs = world.entityDefs.Num();
	while ( slots.Num() < numDefs ) {
		slots.Append( slot_t() );
		seen.Append( -1 );
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
			if ( seen[index] == gatherCount ) {
				continue;
			}
			seen[index] = gatherCount;

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

			// an instance per group of the slot's records
			const int * counts = slot.groups.count;
			for ( int group = 0, first = slot.firstGeometry; group < GPU_SCENE_GROUPS; first += counts[group], group++ ) {
				if ( counts[group] == 0 ) {
					continue;
				}
				gpuSceneInstance_t & instance = instances.Alloc();
				memset( &instance, 0, sizeof( instance ) );
				for ( int r = 0; r < 3; r++ ) {
					for ( int c = 0; c < 4; c++ ) {
						instance.transform[r][c] = def.modelMatrix[c * 4 + r];
					}
				}
				instance.firstGeometry = static_cast<uint32>( first );
				instance.numGeometries = static_cast<uint32>( counts[group] );
				instance.mask = GpuScene_InstanceMask( group, !parms.noShadow );
				instance.skinnedVerts = static_cast<uint32>( slot.groups.skinnedVerts[group] );
				instance.blasKey = slot.blasKey[group];
				if ( skinned ) {
					instance.jointCache = joints->jointsInvertedBuffer;
					instance.numJoints = joints->numInvertedJoints;
				}
			}
		}
	}

	GatherLights( world, view );

	// in index order; of a record two gathers of the frame wrote, the later write stands
	std::stable_sort( writes.Ptr(), writes.Ptr() + writes.Num(), []( const gpuSceneWrite_t & a, const gpuSceneWrite_t & b ) { return a.index < b.index; } );
	int kept = 0;
	for ( int i = 0; i < writes.Num(); i++ ) {
		if ( i + 1 == writes.Num() || writes[i + 1].index != writes[i].index ) {
			writes[kept++] = writes[i];
		}
	}
	writes.SetNum( kept );
	gpuSceneFrame_t * out = static_cast<gpuSceneFrame_t *>( R_ClearedFrameAlloc( sizeof( *out ), FRAME_ALLOC_GPU_SCENE ) );
	out->sceneId = id;
	out->sequence = ++sequence;
	out->base = frameBase;
	out->full = frameFull;
	out->staticGeneration = staticGeneration;
	out->geometryCapacity = geometries.Capacity();
	out->numWrites = writes.Num();
	out->writes = static_cast<gpuSceneWrite_t *>( R_FrameAlloc( Max( 1, writes.Num() ) * sizeof( gpuSceneWrite_t ), FRAME_ALLOC_GPU_SCENE ) );
	memcpy( out->writes, writes.Ptr(), writes.Num() * sizeof( gpuSceneWrite_t ) );
	out->numInstances = instances.Num();
	out->instances = static_cast<gpuSceneInstance_t *>( R_FrameAlloc( Max( 1, instances.Num() ) * sizeof( gpuSceneInstance_t ), FRAME_ALLOC_GPU_SCENE ) );
	memcpy( out->instances, instances.Ptr(), instances.Num() * sizeof( gpuSceneInstance_t ) );
	out->numGeometries = numGeometries;
	out->numLights = lights.Num();
	out->lights = static_cast<gpuSceneLightWrite_t *>( R_FrameAlloc( Max( 1, lights.Num() ) * sizeof( gpuSceneLightWrite_t ), FRAME_ALLOC_GPU_SCENE ) );
	memcpy( out->lights, lights.Ptr(), lights.Num() * sizeof( gpuSceneLightWrite_t ) );

	gathers.Append( { &view, out } );
	view.gpuScene = out;
}

/*
================================================================================================

The back end

================================================================================================
*/

// An image's texture. It may be purged although its material is not, and a record may name one no draw has bound
// yet: Bind loads on demand, so must this.
static textureHandle_t RB_GpuSceneTexture( idImage * image ) {
	if ( image == nullptr ) {
		return textureHandle_t::NONE;
	}
	if ( !image->IsLoaded() ) {
		image->ActuallyLoadImage();
	}
	return image->Handle();
}

void RB_UpdateGpuScene( gpuSceneFrame_t & frame ) {
	for ( int i = 0; i < frame.numWrites; i++ ) {
		gpuSceneWrite_t & write = frame.writes[i];
		const_cast<idMaterial *>( write.material )->EnsureNotPurged();
		for ( int t = 0; t < GPU_SCENE_TEXTURES; t++ ) {
			write.textures[t] = RB_GpuSceneTexture( write.images[t] );
		}
	}
	for ( int i = 0; i < frame.numLights; i++ ) {
		gpuSceneLightWrite_t & write = frame.lights[i];
		const_cast<idMaterial *>( write.material )->EnsureNotPurged();
		for ( int t = 0; t < 2; t++ ) {
			write.textures[t] = RB_GpuSceneTexture( write.images[t] );
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
	static gpuSceneRecovery_t recovery;
	if ( recovery.Asks( frame.sceneId, frame.full, applied ) ) {
		gpuSceneLost.store( frame.sceneId );
	}
	refused += applied ? 0 : 1;

	static int lastPrint = 0;
	if ( r_rtStats.GetBool() && Sys_Milliseconds() - lastPrint >= 1000 ) {
		lastPrint = Sys_Milliseconds();
		common->Printf( "rt scene %u.%u: %d instances, %d geometries, %d written%s, capacity %d, %d lights, %d refused; tlas %d (%d waiting), "
			"blas %d (%d built, %d compacted, %.1f MB), skinned %d (%d verts), %.1f MB per frame\n", frame.sceneId, frame.sequence,
			frame.numInstances, frame.numGeometries, frame.numWrites, frame.full ? " (full)" : "", frame.geometryCapacity, frame.numLights, refused,
			stats.tlasInstances, stats.waiting, stats.blas, stats.built, stats.compacted, stats.blasBytes / ( 1024.0 * 1024.0 ), stats.skinned,
			stats.skinnedVerts, stats.frameBytes / ( 1024.0 * 1024.0 ) );
		refused = 0;
	}
}
