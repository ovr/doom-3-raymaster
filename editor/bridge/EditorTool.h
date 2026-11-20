// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include "EditorHistory.h"

class idEditorWorld;

/*
	The selection and the gizmo that moves or turns it.

	  click              select the entity under the cursor, Ctrl+click adds or removes one
	  drag an arrow      move along that axis, in steps of the grid
	  drag a ring        turn around that axis, in steps of 15 degrees

	The gizmo sits at the first selected entity. A drag changes the entities live; letting go
	makes it one step of the history.
*/
class idEditorTool {
public:
	enum class mode_t { MOVE, ROTATE };

	void				Reset();

	[[nodiscard]] int	NumSelected() const { return selection.Num(); }
	[[nodiscard]] int	Selected( int i ) const { return selection[i]; }
	[[nodiscard]] bool	IsSelected( int index ) const { return selection.FindIndex( index ) != -1; }
	// true when the selection changed
	bool				Select( int index, bool toggle );
	bool				ClearSelection();

	// Brushes are selected instead of entities, never along with them. The face is a side of the
	// one selected brush, or -1.
	struct brushRef_t {
		int					entity = 0;
		int					primitive = 0;
		[[nodiscard]] bool	operator==( const brushRef_t & other ) const { return entity == other.entity && primitive == other.primitive; }
	};
	[[nodiscard]] int	NumSelectedBrushes() const { return brushes.Num(); }
	[[nodiscard]] const brushRef_t &	SelectedBrush( int i ) const { return brushes[i]; }
	[[nodiscard]] int	SelectedFace() const { return ( brushes.Num() == 1 ) ? face : -1; }
	// true when the selection changed
	bool				SelectBrush( const brushRef_t & brush, int newFace, bool toggle );
	// drops the entities an edit removed; true when the selection changed
	bool				DropRemoved( const idEditorWorld & world );

	void				SetMode( mode_t newMode ) { mode = newMode; }
	// 0 = no snapping
	void				SetGridSize( float size ) { gridSize = size; }
	[[nodiscard]] float	GridSize() const { return gridSize; }

	// a press on the gizmo starts a drag: true, and the click is not a selection
	[[nodiscard]] bool	BeginDrag( const idEditorWorld & world, const idVec3 & rayStart, const idVec3 & rayDir, const idVec3 & viewOrigin );
	void				Drag( idEditorWorld & world, const idVec3 & rayStart, const idVec3 & rayDir );
	// true when the drag changed the map: a step was added to the history
	bool				EndDrag( idEditorWorld & world, idEditorHistory & history );
	[[nodiscard]] bool	Dragging() const { return dragAxis != -1; }

	void				Draw( const idEditorWorld & world, const idVec3 & viewOrigin );

private:
	[[nodiscard]] bool	Pivot( const idEditorWorld & world, idVec3 & pivot ) const;
	// the point of the dragged handle the ray is at: along the axis, or the angle around it
	[[nodiscard]] bool	HandleValue( const idVec3 & rayStart, const idVec3 & rayDir, float & value ) const;

	idList<int>			selection;
	idList<brushRef_t>	brushes;
	int					face = -1;
	mode_t				mode = mode_t::MOVE;
	float				gridSize = 8.0f;

	int					dragAxis = -1;			// 0..2 while dragging
	idVec3				dragPivot = vec3_origin;
	float				dragStartValue = 0.0f;
	float				dragSize = 1.0f;		// the gizmo's size when the drag began
	idList<idDict>		dragBefore;				// the map args of the selection, in its order
	bool				dragChanged = false;
};
