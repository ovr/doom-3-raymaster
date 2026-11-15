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
										a jump (CheckEntitySnap) or a new def: snap, drawn at cur
										refs in the areas of prev and cur, the def goes on ticEntities
	frame		SetInterpolation( &x )	alpha, the view; an alpha that falls without a tic snaps the tic
				RenderScene
				  ApplyRenderPoses		render pose -> modelMatrix, modelRenderMatrix,
										inverseBaseModelProject, globalReferenceBounds
				  R_RenderView			the front end reads the pose from the derived data only
				  RestoreTicPoses		derived data from parms again, bit for bit the tic pose
				SetInterpolation( nullptr )

	def->parms and the game's renderEntity_t / renderLight_t always hold the tic pose, so the game
	code between scenes (traces, GuiTrace, sounds) sees the tic poses. The back end reads only the
	copies in the viewEntity_t / viewLight_t, made inside R_RenderView.

	Hooks for the later stages: ticLights and idRenderLightLocal::interp (lights),
	IsViewAttached and renderInterpolation_t::viewAttachedID (view-attached defs, the camera),
	renderInterpStats_t::lights / skeletons (the com_showFPS row).

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
	ApplyEntityPoses( interpolation.alpha );

	// stage 3: the lights of ticLights, the same way
}

/*
===================
idRenderWorldLocal::ApplyEntityPoses
===================
*/
void idRenderWorldLocal::ApplyEntityPoses( const float alpha ) {
	renderInterpStats_t & stats = tr.pc.interp;

	// the debug lines live until the next tic: the drawn poses leave a trail, prev and cur go in once
	const int show = r_showInterpolation.GetInteger();
	const bool showFrame = show > 0 && debugFrameCount != tr.frameCount;
	const bool showTic = showFrame && debugTicCount != ticCount;
	if ( showFrame ) {
		debugFrameCount = tr.frameCount;
	}
	if ( showTic ) {
		debugTicCount = ticCount;
	}

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
			if ( showFrame && show >= 2 ) {
				DebugBox( colorCyan, idBox( def->localReferenceBounds, def->parms.origin, def->parms.axis ) );
			}
			continue;
		}

		if ( interp.snap ) {
			stats.snapped++;
			if ( showTic ) {
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

		if ( showTic ) {
			DebugBox( colorRed, idBox( interp.prevLocalBounds, interp.prevOrigin, interp.prevAxis ) );
			DebugBox( colorGreen, idBox( def->localReferenceBounds, def->parms.origin, def->parms.axis ) );
		}
		if ( showFrame ) {
			DebugBox( colorYellow, idBox( bounds, origin, axis ) );
		}
	}

	// stage 3: with r_showInterpolation 2 also the lights
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

	// stage 3: the lights of ticLights

	posesApplied = false;
}
