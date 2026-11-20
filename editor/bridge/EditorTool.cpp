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
const idVec4		GHOST_COLOR( 1.0f, 1.0f, 1.0f, 1.0f );			// where a dragged brush or face will be
const idVec4		BOX_COLOR( 0.3f, 1.0f, 1.0f, 1.0f );
constexpr float		BOX_HEIGHT_STEPS = 8.0f;		// of the grid
constexpr float		BOX_HEIGHT_NO_GRID = 64.0f;

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
	brushes.Clear();
	face = -1;
	dragging = false;
	dragAxis = -1;
	dragBefore.Clear();
	boxActive = false;
}

float idEditorTool::BoxHeight() const {
	if ( boxHeight > 0.0f ) {
		return boxHeight;
	}
	return ( gridSize > 0.0f ) ? gridSize * BOX_HEIGHT_STEPS : BOX_HEIGHT_NO_GRID;
}

bool idEditorTool::SelectBrush( const brushRef_t & brush, int newFace, bool toggle ) {
	const bool hadEntities = ( selection.Num() > 0 );
	selection.SetNum( 0 );
	if ( toggle ) {
		if ( !brushes.Remove( brush ) ) {
			brushes.Append( brush );
		}
		face = -1;
		return true;
	}
	if ( !hadEntities && brushes.Num() == 1 && brushes[0] == brush && face == newFace ) {
		return false;
	}
	brushes.SetNum( 0 );
	brushes.Append( brush );
	face = newFace;
	return true;
}

bool idEditorTool::Select( int index, bool toggle ) {
	if ( index <= 0 ) {
		return toggle ? false : ClearSelection();
	}
	const bool hadBrushes = ( brushes.Num() > 0 );
	brushes.SetNum( 0 );
	face = -1;
	if ( hadBrushes ) {
		selection.SetNum( 0 );
		selection.Append( index );
		return true;
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
	if ( selection.Num() == 0 && brushes.Num() == 0 ) {
		return false;
	}
	selection.SetNum( 0 );
	brushes.SetNum( 0 );
	face = -1;
	return true;
}

bool idEditorTool::DropRemoved( const idEditorWorld & world ) {
	const int before = selection.Num() + brushes.Num();
	for ( int i = selection.Num() - 1; i >= 0; i-- ) {
		if ( selection[i] >= world.NumEntities() || world.Entity( selection[i] ).removed ) {
			selection.RemoveIndex( i );
		}
	}
	for ( int i = brushes.Num() - 1; i >= 0; i-- ) {
		const brushRef_t & brush = brushes[i];
		if ( brush.entity >= world.NumEntities() || world.Entity( brush.entity ).removed || world.Brush( brush.entity, brush.primitive ) == nullptr ) {
			brushes.RemoveIndex( i );
		}
	}
	if ( brushes.Num() != 1 || face >= world.Brush( brushes[0].entity, brushes[0].primitive )->GetNumSides() ) {
		face = -1;
	}
	return selection.Num() + brushes.Num() != before;
}

bool idEditorTool::Gizmo( const idEditorWorld & world, gizmo_t & gizmo ) const {
	for ( int i = 0; i < 3; i++ ) {
		gizmo.axes[i] = AXES[i];
	}
	gizmo.numAxes = 3;
	if ( selection.Num() > 0 ) {
		if ( selection[0] >= world.NumEntities() ) {
			return false;
		}
		gizmo.pivot = world.Entity( selection[0] ).origin;
		return true;
	}
	idBounds bounds;
	bounds.Clear();
	for ( int i = 0; i < brushes.Num(); i++ ) {
		editorBrushWindings_t windings;
		if ( !world.BrushWindings( brushes[i].entity, brushes[i].primitive, windings ) ) {
			continue;
		}
		if ( SelectedFace() != -1 ) {
			// one arrow, out of the face
			const idFixedWinding & winding = windings[face];
			if ( winding.GetNumPoints() < 3 ) {
				return false;
			}
			gizmo.pivot = winding.GetCenter();
			gizmo.axes[0] = world.Brush( brushes[i].entity, brushes[i].primitive )->GetSide( face )->GetPlane().Normal() * world.Entity( brushes[i].entity ).axis;
			gizmo.numAxes = 1;
			return true;
		}
		bounds.AddBounds( editorBrush::Bounds( windings ) );
	}
	if ( bounds.IsCleared() ) {
		return false;
	}
	gizmo.pivot = bounds.GetCenter();
	return true;
}

bool idEditorTool::HandleValue( const idVec3 & rayStart, const idVec3 & rayDir, float & value ) const {
	const idVec3 & axis = dragGizmo.axes[dragAxis];
	if ( mode == mode_t::MOVE || brushes.Num() > 0 ) {
		float distance;
		return RayNearLine( rayStart, rayDir, dragGizmo.pivot, axis, value, distance );
	}
	idVec3 point;
	if ( !RayOnPlane( rayStart, rayDir, dragGizmo.pivot, axis, point ) ) {
		return false;
	}
	const idVec3 arm = point - dragGizmo.pivot;
	value = RAD2DEG( idMath::ATan( arm * AXES[( dragAxis + 2 ) % 3], arm * AXES[( dragAxis + 1 ) % 3] ) );
	return true;
}

bool idEditorTool::BeginDrag( const idEditorWorld & world, const idVec3 & rayStart, const idVec3 & rayDir, const idVec3 & viewOrigin ) {
	gizmo_t gizmo;
	if ( !Gizmo( world, gizmo ) ) {
		return false;
	}
	const float size = GizmoSize( gizmo.pivot, viewOrigin );
	// brushes are moved only
	const bool move = ( mode == mode_t::MOVE || brushes.Num() > 0 );

	int hitAxis = -1;
	float best = idMath::INFINITY;
	for ( int i = 0; i < gizmo.numAxes; i++ ) {
		float error;
		if ( move ) {
			float along;
			if ( !RayNearLine( rayStart, rayDir, gizmo.pivot, gizmo.axes[i], along, error ) || along < size * ARROW_PICK_START || along > size * ARROW_PICK_END ) {
				continue;
			}
			error /= ARROW_PICK_WIDTH;
		} else {
			idVec3 point;
			if ( !RayOnPlane( rayStart, rayDir, gizmo.pivot, gizmo.axes[i], point ) ) {
				continue;
			}
			error = idMath::Fabs( ( point - gizmo.pivot ).Length() - size ) / RING_PICK_WIDTH;
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
	dragGizmo = gizmo;
	dragSize = size;
	dragDelta = 0.0f;
	dragChanged = false;
	if ( !HandleValue( rayStart, rayDir, dragStartValue ) ) {
		dragAxis = -1;
		return false;
	}
	dragging = true;
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
	const idVec3 & axis = dragGizmo.axes[dragAxis];
	const float delta = ( mode == mode_t::MOVE || brushes.Num() > 0 )
		? Snap( value - dragStartValue, gridSize )
		: Snap( idMath::AngleNormalize180( value - dragStartValue ), ANGLE_SNAP );
	dragDelta = delta;
	dragChanged = ( delta != 0.0f );
	const idVec3 & dragPivot = dragGizmo.pivot;

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
}

idEditorTool::dragResult_t idEditorTool::EndDrag( idEditorWorld & world, idEditorHistory & history ) {
	dragResult_t result;
	if ( !Dragging() ) {
		return result;
	}
	dragging = false;
	if ( brushes.Num() > 0 ) {
		if ( dragChanged ) {
			result.kind = ( SelectedFace() != -1 ) ? dragResult_t::kind_t::FACE : dragResult_t::kind_t::BRUSHES;
			result.delta = dragGizmo.axes[dragAxis] * dragDelta;
			result.distance = dragDelta;
		}
		dragAxis = -1;
		return result;
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
	if ( changes.Num() > 0 ) {
		result.kind = dragResult_t::kind_t::ENTITIES;
	}
	return result;
}

idBounds idEditorTool::Box() const {
	idBounds box;
	box.Clear();
	box.AddPoint( boxStart );
	box.AddPoint( boxEnd );
	idVec3 top = boxStart;
	top[boxAxis] += boxSide * BoxHeight();
	box.AddPoint( top );
	return box;
}

bool idEditorTool::BeginBox( const idEditorWorld & world, const idVec3 & rayStart, const idVec3 & rayDir, const editorPick_t & pick ) {
	// brush planes stay on whole units without a grid too
	const float step = ( gridSize > 0.0f ) ? gridSize : 1.0f;
	idVec3 point;
	boxAxis = 2;
	if ( pick.entity != -1 ) {
		point = rayStart + rayDir * pick.distance;
		const idMapBrush * const brush = world.Brush( pick.entity, pick.primitive );
		if ( brush != nullptr && pick.face >= 0 && pick.face < brush->GetNumSides() ) {
			// on the face: across the axis its normal is nearest to
			const idVec3 normal = brush->GetSide( pick.face )->GetPlane().Normal() * world.Entity( pick.entity ).axis;
			for ( int i = 0; i < 3; i++ ) {
				if ( idMath::Fabs( normal[i] ) > idMath::Fabs( normal[boxAxis] ) ) {
					boxAxis = i;
				}
			}
		}
	} else {
		// the grid plane
		if ( idMath::Fabs( rayDir.z ) < 1.0e-4f || -rayStart.z / rayDir.z <= 0.0f ) {
			return false;
		}
		point = rayStart + rayDir * ( -rayStart.z / rayDir.z );
		point.z = 0.0f;
	}
	for ( int i = 0; i < 3; i++ ) {
		boxStart[i] = Snap( point[i], step );
	}
	boxEnd = boxStart;
	boxSide = ( rayStart[boxAxis] >= boxStart[boxAxis] ) ? 1.0f : -1.0f;
	boxActive = true;
	return true;
}

void idEditorTool::DragBox( const idVec3 & rayStart, const idVec3 & rayDir ) {
	if ( !boxActive || idMath::Fabs( rayDir[boxAxis] ) < 1.0e-4f ) {
		return;
	}
	const float onRay = ( boxStart[boxAxis] - rayStart[boxAxis] ) / rayDir[boxAxis];
	if ( onRay <= 0.0f ) {
		return;
	}
	const float step = ( gridSize > 0.0f ) ? gridSize : 1.0f;
	const idVec3 point = rayStart + rayDir * onRay;
	for ( int i = 0; i < 3; i++ ) {
		boxEnd[i] = ( i == boxAxis ) ? boxStart[i] : Snap( point[i], step );
	}
}

bool idEditorTool::EndBox( idBounds & box ) {
	if ( !boxActive ) {
		return false;
	}
	boxActive = false;
	box = Box();
	return box[1].x > box[0].x && box[1].y > box[0].y && box[1].z > box[0].z;
}

void idEditorTool::Draw( const idEditorWorld & world, const idVec3 & viewOrigin ) {
	idRenderWorld * const renderWorld = world.RenderWorld();
	for ( int i = 0; i < selection.Num(); i++ ) {
		if ( selection[i] < world.NumEntities() ) {
			renderWorld->DebugBounds( SELECTION_COLOR, world.WorldBounds( selection[i] ) );
		}
	}
	// the edges of the selected brushes; the selected face over them
	for ( int i = 0; i < brushes.Num(); i++ ) {
		editorBrushWindings_t windings;
		if ( !world.BrushWindings( brushes[i].entity, brushes[i].primitive, windings ) ) {
			continue;
		}
		for ( int pass = 0; pass < 2; pass++ ) {
			for ( int side = 0; side < windings.Num(); side++ ) {
				if ( ( side == SelectedFace() ) != ( pass == 1 ) ) {
					continue;
				}
				const idFixedWinding & winding = windings[side];
				for ( int j = 0; j < winding.GetNumPoints(); j++ ) {
					renderWorld->DebugLine( ( pass == 1 ) ? ACTIVE_COLOR : SELECTION_COLOR, winding[j].ToVec3(), winding[( j + 1 ) % winding.GetNumPoints()].ToVec3() );
				}
			}
		}
	}

	// where the dragged brushes, or the dragged face, will be
	if ( Dragging() && brushes.Num() > 0 && dragDelta != 0.0f ) {
		const idVec3 offset = dragGizmo.axes[dragAxis] * dragDelta;
		for ( int i = 0; i < brushes.Num(); i++ ) {
			editorBrushWindings_t windings;
			if ( !world.BrushWindings( brushes[i].entity, brushes[i].primitive, windings ) ) {
				continue;
			}
			for ( int side = 0; side < windings.Num(); side++ ) {
				if ( SelectedFace() != -1 && side != SelectedFace() ) {
					continue;
				}
				const idFixedWinding & winding = windings[side];
				for ( int j = 0; j < winding.GetNumPoints(); j++ ) {
					renderWorld->DebugLine( GHOST_COLOR, winding[j].ToVec3() + offset, winding[( j + 1 ) % winding.GetNumPoints()].ToVec3() + offset );
				}
			}
		}
	}
	if ( boxActive ) {
		renderWorld->DebugBounds( BOX_COLOR, Box() );
	}

	gizmo_t gizmo;
	if ( Dragging() ) {
		// while dragging the gizmo stays where and as big as it was grabbed
		gizmo = dragGizmo;
	} else if ( !Gizmo( world, gizmo ) ) {
		return;
	}
	const float size = Dragging() ? dragSize : GizmoSize( gizmo.pivot, viewOrigin );
	for ( int i = 0; i < gizmo.numAxes; i++ ) {
		const idVec4 & color = ( i == dragAxis ) ? ACTIVE_COLOR : ( ( gizmo.numAxes == 1 ) ? BOX_COLOR : AXIS_COLORS[i] );
		if ( mode == mode_t::MOVE || brushes.Num() > 0 ) {
			renderWorld->DebugArrow( color, gizmo.pivot, gizmo.pivot + gizmo.axes[i] * size, Max( 1, static_cast<int>( size * 0.12f ) ) );
		} else {
			renderWorld->DebugCircle( color, gizmo.pivot, gizmo.axes[i], size, RING_STEPS );
		}
	}
}
