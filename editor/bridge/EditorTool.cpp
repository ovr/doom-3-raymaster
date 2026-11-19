// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include "EditorTool.h"
#include "EditorWorld.h"

namespace {

constexpr float		GIZMO_VIEW_SCALE = 0.15f;		// the gizmo's size as a part of its distance
constexpr float		GIZMO_MIN_SIZE = 4.0f;
constexpr float		ARROW_PICK_START = 0.15f;		// in gizmo sizes: the part of an arrow that takes a press
constexpr float		ARROW_PICK_END = 1.1f;
constexpr float		ARROW_PICK_WIDTH = 0.08f;
constexpr float		RING_PICK_WIDTH = 0.1f;
constexpr float		ANGLE_SNAP = 15.0f;
constexpr int		RING_STEPS = 48;

const idVec3		AXES[3] = { idVec3( 1.0f, 0.0f, 0.0f ), idVec3( 0.0f, 1.0f, 0.0f ), idVec3( 0.0f, 0.0f, 1.0f ) };
const idVec4		AXIS_COLORS[3] = { idVec4( 1.0f, 0.25f, 0.25f, 1.0f ), idVec4( 0.3f, 1.0f, 0.3f, 1.0f ), idVec4( 0.35f, 0.55f, 1.0f, 1.0f ) };
const idVec4		ACTIVE_COLOR( 1.0f, 1.0f, 0.2f, 1.0f );
const idVec4		SELECTION_COLOR( 1.0f, 0.75f, 0.1f, 1.0f );

[[nodiscard]] float GizmoSize( const idVec3 & pivot, const idVec3 & viewOrigin ) {
	return Max( GIZMO_MIN_SIZE, ( pivot - viewOrigin ).Length() * GIZMO_VIEW_SCALE );
}

// Where the ray passes nearest to the line through origin along the unit vector axis:
// along = the place on the line, distance = how far the ray is from it there.
[[nodiscard]] bool RayNearLine( const idVec3 & rayStart, const idVec3 & rayDir, const idVec3 & origin, const idVec3 & axis, float & along, float & distance ) {
	const idVec3 w = rayStart - origin;
	const float b = rayDir * axis;
	const float denom = 1.0f - b * b;
	if ( denom < 1.0e-5f ) {
		return false;
	}
	const float onRay = ( b * ( axis * w ) - ( rayDir * w ) ) / denom;
	if ( onRay <= 0.0f ) {
		return false;
	}
	along = ( axis * w ) + b * onRay;
	distance = ( ( rayStart + rayDir * onRay ) - ( origin + axis * along ) ).Length();
	return true;
}

[[nodiscard]] bool RayOnPlane( const idVec3 & rayStart, const idVec3 & rayDir, const idVec3 & origin, const idVec3 & normal, idVec3 & point ) {
	const float denom = rayDir * normal;
	if ( idMath::Fabs( denom ) < 1.0e-4f ) {
		return false;
	}
	const float onRay = ( ( origin - rayStart ) * normal ) / denom;
	if ( onRay <= 0.0f ) {
		return false;
	}
	point = rayStart + rayDir * onRay;
	return true;
}

// right-handed turn of v around the unit vector axis
[[nodiscard]] idVec3 Turn( const idVec3 & v, const idVec3 & axis, float degrees ) {
	float s, c;
	idMath::SinCos( DEG2RAD( degrees ), s, c );
	return v * c + axis.Cross( v ) * s + axis * ( ( axis * v ) * ( 1.0f - c ) );
}

[[nodiscard]] idMat3 EntityAxis( const idDict & args ) {
	idMat3 axis;
	const char * const key = args.FindKey( "light_rotation" ) ? "light_rotation" : "rotation";
	if ( !args.GetMatrix( key, "1 0 0 0 1 0 0 0 1", axis ) ) {
		axis = idAngles( 0.0f, args.GetFloat( "angle" ), 0.0f ).ToMat3();
	}
	return axis;
}

[[nodiscard]] float Snap( float value, float step ) {
	return ( step > 0.0f ) ? idMath::Rint( value / step ) * step : value;
}

}	// namespace

void idEditorTool::Reset() {
	selection.Clear();
	dragAxis = -1;
	dragBefore.Clear();
}

bool idEditorTool::Select( int index, bool toggle ) {
	if ( index <= 0 ) {
		return toggle ? false : ClearSelection();
	}
	if ( toggle ) {
		if ( !selection.Remove( index ) ) {
			selection.Append( index );
		}
		return true;
	}
	if ( selection.Num() == 1 && selection[0] == index ) {
		return false;
	}
	selection.SetNum( 0 );
	selection.Append( index );
	return true;
}

bool idEditorTool::ClearSelection() {
	if ( selection.Num() == 0 ) {
		return false;
	}
	selection.SetNum( 0 );
	return true;
}

bool idEditorTool::Pivot( const idEditorWorld & world, idVec3 & pivot ) const {
	if ( selection.Num() == 0 || selection[0] >= world.NumEntities() ) {
		return false;
	}
	pivot = world.Entity( selection[0] ).origin;
	return true;
}

bool idEditorTool::HandleValue( const idVec3 & rayStart, const idVec3 & rayDir, float & value ) const {
	const idVec3 & axis = AXES[dragAxis];
	if ( mode == mode_t::MOVE ) {
		float distance;
		return RayNearLine( rayStart, rayDir, dragPivot, axis, value, distance );
	}
	idVec3 point;
	if ( !RayOnPlane( rayStart, rayDir, dragPivot, axis, point ) ) {
		return false;
	}
	const idVec3 arm = point - dragPivot;
	value = RAD2DEG( idMath::ATan( arm * AXES[( dragAxis + 2 ) % 3], arm * AXES[( dragAxis + 1 ) % 3] ) );
	return true;
}

bool idEditorTool::BeginDrag( const idEditorWorld & world, const idVec3 & rayStart, const idVec3 & rayDir, const idVec3 & viewOrigin ) {
	idVec3 pivot;
	if ( !Pivot( world, pivot ) ) {
		return false;
	}
	const float size = GizmoSize( pivot, viewOrigin );

	int hitAxis = -1;
	float best = idMath::INFINITY;
	for ( int i = 0; i < 3; i++ ) {
		float error;
		if ( mode == mode_t::MOVE ) {
			float along;
			if ( !RayNearLine( rayStart, rayDir, pivot, AXES[i], along, error ) || along < size * ARROW_PICK_START || along > size * ARROW_PICK_END ) {
				continue;
			}
			error /= ARROW_PICK_WIDTH;
		} else {
			idVec3 point;
			if ( !RayOnPlane( rayStart, rayDir, pivot, AXES[i], point ) ) {
				continue;
			}
			error = idMath::Fabs( ( point - pivot ).Length() - size ) / RING_PICK_WIDTH;
		}
		if ( error < size && error < best ) {
			best = error;
			hitAxis = i;
		}
	}
	if ( hitAxis == -1 ) {
		return false;
	}

	dragAxis = hitAxis;
	dragPivot = pivot;
	dragSize = size;
	dragChanged = false;
	if ( !HandleValue( rayStart, rayDir, dragStartValue ) ) {
		dragAxis = -1;
		return false;
	}
	dragBefore.SetNum( selection.Num() );
	for ( int i = 0; i < selection.Num(); i++ ) {
		dragBefore[i] = world.MapArgs( selection[i] );
	}
	return true;
}

void idEditorTool::Drag( idEditorWorld & world, const idVec3 & rayStart, const idVec3 & rayDir ) {
	float value;
	if ( !Dragging() || !HandleValue( rayStart, rayDir, value ) ) {
		return;
	}
	const idVec3 & axis = AXES[dragAxis];
	const float delta = ( mode == mode_t::MOVE )
		? Snap( value - dragStartValue, gridSize )
		: Snap( idMath::AngleNormalize180( value - dragStartValue ), ANGLE_SNAP );

	for ( int i = 0; i < selection.Num(); i++ ) {
		const idDict & before = dragBefore[i];
		// a brush entity without an origin is where its brushes are: not movable by a key
		if ( before.FindKey( "origin" ) == nullptr ) {
			continue;
		}
		idDict args = before;
		const idVec3 origin = before.GetVector( "origin", "0 0 0" );
		if ( mode == mode_t::MOVE ) {
			args.SetVector( "origin", origin + axis * delta );
		} else {
			idMat3 entityAxis = EntityAxis( before );
			for ( int row = 0; row < 3; row++ ) {
				entityAxis[row] = Turn( entityAxis[row], axis, delta );
			}
			args.SetVector( "origin", dragPivot + Turn( origin - dragPivot, axis, delta ) );
			args.SetMatrix( before.FindKey( "light_rotation" ) ? "light_rotation" : "rotation", entityAxis );
			args.Delete( "angle" );
		}
		world.SetMapArgs( selection[i], args );
	}
	dragChanged = ( delta != 0.0f );
}

bool idEditorTool::EndDrag( idEditorWorld & world, idEditorHistory & history ) {
	if ( !Dragging() ) {
		return false;
	}
	idList<editorChange_t> changes;
	for ( int i = 0; i < selection.Num(); i++ ) {
		if ( !dragChanged ) {
			// a drag that came back to its start leaves the keys exactly as they were
			world.SetMapArgs( selection[i], dragBefore[i] );
			continue;
		}
		editorChange_t & change = changes.Alloc();
		change.index = selection[i];
		change.before = dragBefore[i];
		change.after = world.MapArgs( selection[i] );
	}
	history.Add( ( mode == mode_t::MOVE ) ? "Move" : "Rotate", changes );

	dragAxis = -1;
	dragBefore.Clear();
	return changes.Num() > 0;
}

void idEditorTool::Draw( const idEditorWorld & world, const idVec3 & viewOrigin ) {
	idRenderWorld * const renderWorld = world.RenderWorld();
	for ( int i = 0; i < selection.Num(); i++ ) {
		if ( selection[i] < world.NumEntities() ) {
			renderWorld->DebugBounds( SELECTION_COLOR, world.WorldBounds( selection[i] ) );
		}
	}

	idVec3 pivot;
	if ( !Pivot( world, pivot ) ) {
		return;
	}
	// while dragging the gizmo stays where and as big as it was grabbed
	const idVec3 center = Dragging() ? dragPivot : pivot;
	const float size = Dragging() ? dragSize : GizmoSize( pivot, viewOrigin );
	for ( int i = 0; i < 3; i++ ) {
		const idVec4 & color = ( i == dragAxis ) ? ACTIVE_COLOR : AXIS_COLORS[i];
		if ( mode == mode_t::MOVE ) {
			renderWorld->DebugArrow( color, center, center + AXES[i] * size, Max( 1, static_cast<int>( size * 0.12f ) ) );
		} else {
			renderWorld->DebugCircle( color, center, AXES[i], size, RING_STEPS );
		}
	}
}
