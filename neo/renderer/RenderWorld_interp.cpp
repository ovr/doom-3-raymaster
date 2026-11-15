/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "../idlib/precompiled.h"
#pragma hdrstop

#include "tr_local.h"

/*
===============================================================================

	Render interpolation (r_interpolate)

	The game moves the defs once per tic, the frames come faster or slower. A def that moved
	in the last tic is drawn between its pose after the tic before (prev) and its tic pose
	(cur), at the alpha of the frame (RenderInterpolation.h has the math):

	game tic	BeginGameTic			ticCount++, ticEntities / ticLights emptied
				Update*Def				first update of the tic: prev = the pose before it, snap = false
										a jump (Check*Snap) or a new def: snap, drawn at cur; a light
										also snaps on a new shape, a fog, blend or parallel light always
										refs in the areas of prev and cur, the def goes on ticEntities / ticLights
	frame		SetInterpolation( &x )	alpha, the view; an alpha that falls without a tic snaps the tic
				RenderScene
				  ApplyRenderPoses		render pose -> entities: modelMatrix, modelRenderMatrix,
										inverseBaseModelProject, globalReferenceBounds; lights: lightProject,
										baseLightProject, inverseBaseLightProject, globalLightOrigin, globalLightBounds
				  R_RenderView			the front end reads the pose from the derived data only
				  RestoreTicPoses		derived data from parms again, bit for bit the tic pose
				SetInterpolation( nullptr )

	def->parms and the game's renderEntity_t / renderLight_t always hold the tic pose, so the game
	code between scenes (traces, GuiTrace, sounds) sees the tic poses. The back end reads only the
	copies in the viewEntity_t / viewLight_t, made inside R_RenderView.

	A light that moved has no per-pose caches: UpdateLightDef drops its prelight shadows (lightHasMoved)
	and R_FreeLightDefDerivedData its static interactions, so its interactions and shadow volumes are
	made every frame from the drawn pose. Its areaNum (the area culling) stays the one of the tic pose.

	Hooks for the later stages: IsViewAttached and renderInterpolation_t::viewAttachedID (view-attached
	defs, the camera), PoseLight and AddLightPoseRefs (view-attached lights placed at the drawn view),
	renderInterpStats_t::skeletons (the com_showFPS row).

===============================================================================
*/

/*
===================
R_EntityPoseChanged
===================
*/
static bool R_EntityPoseChanged( const idRenderEntityLocal * def ) {
	const defInterp_t & interp = def->interp;
	return interp.prevOrigin != def->parms.origin || interp.prevAxis != def->parms.axis || interp.prevLocalBounds != def->localReferenceBounds;
}

/*
===================
R_EntityDrawBounds

The local bounds of the drawn pose: a lerped skeleton spans both tic poses.
===================
*/
static idBounds R_EntityDrawBounds( const idRenderEntityLocal * def ) {
	idBounds bounds = def->localReferenceBounds;
	bounds.AddBounds( def->interp.prevLocalBounds );
	return bounds;
}

/*
===================
R_LightPoseChanged
===================
*/
static bool R_LightPoseChanged( const idRenderLightLocal * light ) {
	const defInterp_t & interp = light->interp;
	return interp.prevOrigin != light->parms.origin || interp.prevAxis != light->parms.axis;
}

/*
===================
R_LightDefAt
===================
*/
static idRenderLightLocal * R_LightDefAt( const idList<idRenderLightLocal *, TAG_LIGHT> & lightDefs, const int index ) {
	return ( index >= 0 && index < lightDefs.Num() ) ? lightDefs[index] : nullptr;
}

/*
===================
idRenderWorldLocal::InterpolatesDefs
===================
*/
bool idRenderWorldLocal::InterpolatesDefs() const {
	// only a world that gets game tics (gameRenderWorld) keeps the lists
	return ticCount > 0 && r_interpolate.GetBool();
}

/*
===================
idRenderWorldLocal::ClearInterpolation
===================
*/
void idRenderWorldLocal::ClearInterpolation() {
	ticEntities.Clear();
	ticLights.Clear();
	posedLights.Clear();
	interpOn = false;
	posesApplied = false;
	ticSnapAll = false;
	ticMaxAlpha = 0.0f;
}

/*
===================
idRenderWorldLocal::BeginGameTic
===================
*/
void idRenderWorldLocal::BeginGameTic() {
	// RenderScene always restores, this only guards against a scene that was cut short
	if ( posesApplied ) {
		RestoreTicPoses();
	}

	ticCount++;
	ticEntities.SetNum( 0 );
	ticLights.SetNum( 0 );
	ticSnapAll = false;
	ticMaxAlpha = 0.0f;
}

/*
===================
idRenderWorldLocal::SetInterpolation
===================
*/
void idRenderWorldLocal::SetInterpolation( const renderInterpolation_t * interp ) {
	if ( interp == nullptr ) {
		interpOn = false;
		return;
	}

	interpolation = *interp;

	// NaN draws the tic pose
	const float alpha = !( interpolation.alpha < 1.0f ) ? 1.0f : Max( interpolation.alpha, 0.0f );
	interpolation.alpha = alpha;

	// the frame loop restarted the tic clock without a tic (the sync after a long frame): the tic
	// pose was drawn already, going back towards prev would jerk; 0.1 absorbs a changed tic length
	if ( alpha + 0.1f < ticMaxAlpha ) {
		ticSnapAll = true;
	}
	ticMaxAlpha = Max( ticMaxAlpha, alpha );

	interpOn = true;
}

/*
===================
idRenderWorldLocal::BeginEntityTic

Called by UpdateEntityDef before its early outs, so the first update of a tic sees the pose of the tic before.
===================
*/
void idRenderWorldLocal::BeginEntityTic( idRenderEntityLocal * def, const renderEntity_t * re ) {
	defInterp_t & interp = def->interp;
	if ( interp.ticStamp == ticCount ) {
		return;
	}

	interp.prevOrigin = def->parms.origin;
	interp.prevAxis = def->parms.axis;
	interp.prevLocalBounds = def->localReferenceBounds;
	interp.prevModel = def->parms.hModel;
	interp.ticStamp = ticCount;
	interp.snap = false;
	interp.listed = false;

	// a view-attached def follows the drawn view every frame, moved or not
	if ( re->viewAttachedID != 0 && InterpolatesDefs() ) {
		interp.listed = true;
		ticEntities.Append( def->index );
	}
}

/*
===================
idRenderWorldLocal::CheckEntitySnap

After def->parms = *re; sticky for the rest of the tic.
===================
*/
void idRenderWorldLocal::CheckEntitySnap( idRenderEntityLocal * def ) const {
	defInterp_t & interp = def->interp;
	if ( interp.snap ) {
		return;
	}

	// a callback def gets its model from the callback, the update passes NULL
	if ( def->parms.hModel != nullptr && def->parms.hModel != interp.prevModel ) {
		interp.snap = true;
		return;
	}

	interp.snap = R_PoseJumped( interp.prevOrigin, interp.prevAxis, def->parms.origin, def->parms.axis, r_interpolateSnapDist.GetFloat() );
}

/*
===================
idRenderWorldLocal::ListEntityDef

After R_CreateEntityRefs. Snapped defs are listed too, the stats and r_showInterpolation count them.
===================
*/
void idRenderWorldLocal::ListEntityDef( idRenderEntityLocal * def ) {
	defInterp_t & interp = def->interp;
	if ( interp.listed || interp.ticStamp != ticCount || def->entityRefs == nullptr || !InterpolatesDefs() ) {
		return;
	}
	if ( !R_EntityPoseChanged( def ) ) {
		return;
	}

	interp.listed = true;
	ticEntities.Append( def->index );
}

/*
===================
idRenderWorldLocal::PrevPoseNeedsRefs

R_CreateEntityRefs: the drawn pose can leave the areas of the tic pose. bounds = the local bounds to push at prev.
===================
*/
bool idRenderWorldLocal::PrevPoseNeedsRefs( const idRenderEntityLocal * def, idBounds & bounds ) const {
	const defInterp_t & interp = def->interp;
	if ( interp.snap || interp.ticStamp != ticCount || !InterpolatesDefs() ) {
		return false;
	}

	bounds = R_EntityDrawBounds( def );

	// the areas of the tic pose cover a move of less than a unit
	return ( def->parms.origin - interp.prevOrigin ).LengthSqr() > 1.0f
		|| !def->parms.axis.Compare( interp.prevAxis, 1e-3f )
		|| !bounds.Compare( def->localReferenceBounds, 1.0f );
}

/*
===================
idRenderWorldLocal::IsViewAttached
===================
*/
bool idRenderWorldLocal::IsViewAttached( const idRenderEntityLocal * def ) const {
	return def->parms.viewAttachedID != 0 && def->parms.viewAttachedID == interpolation.viewAttachedID;
}

/*
===================
idRenderWorldLocal::BeginLightTic

Called by UpdateLightDef before light->parms changes, so the first update of a tic sees the pose of the tic before.
===================
*/
void idRenderWorldLocal::BeginLightTic( idRenderLightLocal * light, const renderLight_t * rlight ) {
	defInterp_t & interp = light->interp;
	if ( interp.ticStamp == ticCount ) {
		return;
	}

	// ticStamp -1: a new def, snapped at its first pose
	const bool isNew = interp.ticStamp < 0;
	interp.prevOrigin = isNew ? rlight->origin : light->parms.origin;
	interp.prevAxis = isNew ? rlight->axis : light->parms.axis;
	interp.ticStamp = ticCount;
	interp.snap = isNew;
	interp.listed = false;

	// a view-attached light follows the drawn view every frame, moved or not, new or not
	if ( rlight->viewAttachedID != 0 && InterpolatesDefs() ) {
		interp.listed = true;
		ticLights.Append( light->index );
	}
}

/*
===================
idRenderWorldLocal::CheckLightSnap

Before light->parms = *rlight; sticky for the rest of the tic.
===================
*/
void idRenderWorldLocal::CheckLightSnap( idRenderLightLocal * light, const renderLight_t * rlight ) const {
	defInterp_t & interp = light->interp;
	if ( interp.snap ) {
		return;
	}

	// a new radius, frustum, shader or type would morph the volume
	if ( !R_LightShapeMatches( light->parms, *rlight, light->lightHasMoved ) ) {
		interp.snap = true;
		return;
	}

	// a fog light closes portals for its tic pose (R_CreateLightDefFogPortals); blend and parallel lights stay too
	const idMaterial * shader = ( rlight->shader != nullptr ) ? rlight->shader : light->lightShader;
	if ( rlight->parallel || shader == nullptr || shader->IsFogLight() || shader->IsBlendLight() ) {
		interp.snap = true;
		return;
	}

	interp.snap = R_PoseJumped( interp.prevOrigin, interp.prevAxis, rlight->origin, rlight->axis, r_interpolateSnapDist.GetFloat() );
}

/*
===================
idRenderWorldLocal::ListLightDef

At the end of UpdateLightDef. A light that didn't move this tic (a static light) is never listed.
===================
*/
void idRenderWorldLocal::ListLightDef( idRenderLightLocal * light ) {
	defInterp_t & interp = light->interp;
	if ( interp.listed || interp.ticStamp != ticCount || light->references == nullptr || !InterpolatesDefs() ) {
		return;
	}
	if ( !R_LightPoseChanged( light ) ) {
		return;
	}

	interp.listed = true;
	ticLights.Append( light->index );
}

/*
===================
idRenderWorldLocal::PrevPoseNeedsRefs

R_CreateLightRefs: the drawn pose can leave the areas of the tic pose.
===================
*/
bool idRenderWorldLocal::PrevPoseNeedsRefs( const idRenderLightLocal * light ) const {
	const defInterp_t & interp = light->interp;
	if ( interp.snap || interp.ticStamp != ticCount || !InterpolatesDefs() ) {
		return false;
	}

	// the areas of the tic pose cover a move of less than a unit
	return ( light->parms.origin - interp.prevOrigin ).LengthSqr() > 1.0f || !light->parms.axis.Compare( interp.prevAxis, 1e-3f );
}

/*
===================
idRenderWorldLocal::IsViewAttached
===================
*/
bool idRenderWorldLocal::IsViewAttached( const idRenderLightLocal * light ) const {
	return light->parms.viewAttachedID != 0 && light->parms.viewAttachedID == interpolation.viewAttachedID;
}

/*
===================
idRenderWorldLocal::PoseLight

The derived pose data of the light at origin and axis until RestoreTicPoses. Prelight shadows and static
interactions were made for the tic pose: UpdateLightDef drops them when a light moves, a light that has them stays.
===================
*/
bool idRenderWorldLocal::PoseLight( idRenderLightLocal * light, const idVec3 & origin, const idMat3 & axis ) {
	assert( posesApplied );
	if ( light->parms.prelightModel != nullptr || light->firstInteraction != nullptr ) {
		return false;
	}

	R_DeriveLightData( light, origin, axis );
	if ( !light->interp.posed ) {
		light->interp.posed = true;
		posedLights.Append( light->index );
	}
	return true;
}

/*
===================
idRenderWorldLocal::AddLightPoseRefs

Kept until the light next moves; inside a scene only before R_RenderView, which reads the refs in parallel.
===================
*/
void idRenderWorldLocal::AddLightPoseRefs( idRenderLightLocal * light, const idVec3 & origin, const idMat3 & axis ) {
	lightPose_t pose;
	R_DeriveLightData( light->parms, origin, axis, pose );

	// AddLightRefToArea skips the areas the light is in already
	tr.viewCount++;
	PushFrustumIntoTree( nullptr, light, pose.inverseBaseLightProject, bounds_zeroOneCube );
}

/*
===================
idRenderWorldLocal::ApplyRenderPoses

RenderScene, before R_RenderView.
===================
*/
void idRenderWorldLocal::ApplyRenderPoses() {
	if ( !interpOn || posesApplied || !r_interpolate.GetBool() ) {
		return;
	}

	// the last interpolated scene of the frame
	renderInterpStats_t & stats = tr.pc.interp;
	stats = renderInterpStats_t();
	stats.active = true;
	stats.alpha = ticSnapAll ? 1.0f : interpolation.alpha;
	stats.listed = ticEntities.Num();

	if ( stats.alpha >= 1.0f ) {
		return;
	}

	posesApplied = true;

	// the debug lines live until the next tic: the drawn poses leave a trail, prev and cur go in once
	interpShow_t show;
	show.level = r_showInterpolation.GetInteger();
	show.frame = show.level > 0 && debugFrameCount != tr.frameCount;
	show.tic = show.frame && debugTicCount != ticCount;
	if ( show.frame ) {
		debugFrameCount = tr.frameCount;
	}
	if ( show.tic ) {
		debugTicCount = ticCount;
	}

	ApplyEntityPoses( interpolation.alpha, show );
	ApplyLightPoses( interpolation.alpha, show );
}

/*
===================
idRenderWorldLocal::ApplyEntityPoses
===================
*/
void idRenderWorldLocal::ApplyEntityPoses( const float alpha, const interpShow_t & show ) {
	renderInterpStats_t & stats = tr.pc.interp;

	for ( int i = 0; i < ticEntities.Num(); i++ ) {
		const int index = ticEntities[i];
		idRenderEntityLocal * def = ( index >= 0 && index < entityDefs.Num() ) ? entityDefs[index] : nullptr;

		// freed after the update, or a new def in the handle; a handle can be listed twice
		if ( def == nullptr || !def->interp.listed || def->interp.ticStamp != ticCount || def->interp.posed ) {
			continue;
		}
		defInterp_t & interp = def->interp;

		if ( IsViewAttached( def ) ) {
			// stage 4 poses these relative to the drawn view
			stats.attached++;
			if ( show.frame && show.level >= 2 ) {
				DebugBox( colorCyan, idBox( def->localReferenceBounds, def->parms.origin, def->parms.axis ) );
			}
			continue;
		}

		if ( interp.snap ) {
			stats.snapped++;
			if ( show.tic ) {
				DebugBox( colorGreen, idBox( def->localReferenceBounds, def->parms.origin, def->parms.axis ) );
			}
			continue;
		}

		// static interactions were made for the tic pose; an update frees them, so only a def
		// listed for its view attachment can still have them
		if ( def->entityRefs == nullptr || def->firstInteraction != nullptr || !R_EntityPoseChanged( def ) ) {
			continue;
		}

		const idVec3 origin = interp.prevOrigin + ( def->parms.origin - interp.prevOrigin ) * alpha;
		const idMat3 axis = R_LerpAxis( interp.prevAxis, def->parms.axis, alpha );
		const idBounds bounds = R_EntityDrawBounds( def );

		R_DeriveEntityData( origin, axis, bounds, def->modelMatrix, def->modelRenderMatrix, def->inverseBaseModelProject, def->globalReferenceBounds );
		interp.posed = true;
		stats.entities++;

		if ( show.tic ) {
			DebugBox( colorRed, idBox( interp.prevLocalBounds, interp.prevOrigin, interp.prevAxis ) );
			DebugBox( colorGreen, idBox( def->localReferenceBounds, def->parms.origin, def->parms.axis ) );
		}
		if ( show.frame ) {
			DebugBox( colorYellow, idBox( bounds, origin, axis ) );
		}
	}
}

/*
===================
idRenderWorldLocal::ApplyLightPoses
===================
*/
void idRenderWorldLocal::ApplyLightPoses( const float alpha, const interpShow_t & show ) {
	renderInterpStats_t & stats = tr.pc.interp;
	const bool showLights = show.level >= 2;

	for ( int i = 0; i < ticLights.Num(); i++ ) {
		idRenderLightLocal * light = R_LightDefAt( lightDefs, ticLights[i] );

		// freed after the update, or a new def in the handle; a handle can be listed twice
		if ( light == nullptr || !light->interp.listed || light->interp.ticStamp != ticCount || light->interp.posed ) {
			continue;
		}
		const defInterp_t & interp = light->interp;

		if ( IsViewAttached( light ) ) {
			// stage 4 places these relative to the drawn view: PoseLight, and AddLightPoseRefs for the areas
			stats.attached++;
			if ( show.frame && showLights ) {
				DebugLightPose( colorCyan, light, light->parms.origin, light->parms.axis );
			}
			continue;
		}

		if ( interp.snap ) {
			if ( show.tic && showLights ) {
				DebugLightPose( colorGreen, light, light->parms.origin, light->parms.axis );
			}
			continue;
		}

		if ( light->references == nullptr || !R_LightPoseChanged( light ) ) {
			continue;
		}

		const idVec3 origin = interp.prevOrigin + ( light->parms.origin - interp.prevOrigin ) * alpha;
		const idMat3 axis = R_LerpAxis( interp.prevAxis, light->parms.axis, alpha );

		if ( !PoseLight( light, origin, axis ) ) {
			continue;
		}
		stats.lights++;

		if ( show.tic && showLights ) {
			DebugLightPose( colorRed, light, interp.prevOrigin, interp.prevAxis );
			DebugLightPose( colorGreen, light, light->parms.origin, light->parms.axis );
		}
		if ( show.frame && showLights ) {
			DebugLightPose( colorYellow, light, origin, axis );
		}
	}
}

/*
===================
idRenderWorldLocal::DebugLightPose

r_showInterpolation 2: a small box on the light origin and a line along its projection (point lights: axis[0]).
===================
*/
void idRenderWorldLocal::DebugLightPose( const idVec4 & color, const idRenderLightLocal * light, const idVec3 & origin, const idMat3 & axis ) {
	DebugBox( color, idBox( origin, idVec3( 4.0f, 4.0f, 4.0f ), axis ) );

	idVec3 dir = ( light->parms.pointLight || light->parms.parallel ) ? axis[0] : axis * light->parms.target;
	if ( dir.Normalize() > 0.0f ) {
		DebugLine( color, origin, origin + dir * 24.0f );
	}
}

/*
===================
idRenderWorldLocal::RestoreTicPoses

RenderScene, after R_RenderPostProcess.
===================
*/
void idRenderWorldLocal::RestoreTicPoses() {
	if ( !posesApplied ) {
		return;
	}

	for ( int i = 0; i < ticEntities.Num(); i++ ) {
		const int index = ticEntities[i];
		idRenderEntityLocal * def = ( index >= 0 && index < entityDefs.Num() ) ? entityDefs[index] : nullptr;
		if ( def == nullptr || !def->interp.posed ) {
			continue;
		}
		R_DeriveEntityData( def );
		def->interp.posed = false;
	}

	for ( int i = 0; i < posedLights.Num(); i++ ) {
		idRenderLightLocal * light = R_LightDefAt( lightDefs, posedLights[i] );
		if ( light == nullptr || !light->interp.posed ) {
			continue;
		}
		R_DeriveLightData( light, light->parms.origin, light->parms.axis );
		light->interp.posed = false;
	}
	posedLights.SetNum( 0 );

	posesApplied = false;
}
