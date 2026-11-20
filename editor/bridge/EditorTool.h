// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include "EditorHistory.h"

class idEditorWorld;
struct editorPick_t;

/*
	The selection, the gizmo that moves or turns it, and the box tool.

	  click              select what is under the cursor, Ctrl+click adds or removes one
	  drag an arrow      move along that axis, in steps of the grid
	  drag a ring        turn around that axis, in steps of 15 degrees (entities only)
	  box tool: drag     a rectangle on the face or the grid plane under the cursor; letting go
	                     makes a brush of it, as high as the box height

	The gizmo sits at the first selected entity, in the middle of the selected brushes, or as one
	arrow along its normal on the selected face. A drag changes entities live; of brushes and a
	face it shows the outline where they will be. Letting go makes it one step of the history.
*/
class idEditorTool {
public:
	enum class mode_t { MOVE, ROTATE };
	enum class tool_t { SELECT, BOX };

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
	[[nodiscard]] bool	IsBrushSelected( const brushRef_t & brush ) const { return brushes.FindIndex( brush ) != -1; }
	[[nodiscard]] int	SelectedFace() const { return ( brushes.Num() == 1 ) ? face : -1; }
	// true when the selection changed
	bool				SelectBrush( const brushRef_t & brush, int newFace, bool toggle );
	// drops the entities an edit removed; true when the selection changed
	bool				DropRemoved( const idEditorWorld & world );

	void				SetMode( mode_t newMode ) { mode = newMode; }
	void				SetTool( tool_t newTool ) { tool = newTool; boxActive = false; }
	[[nodiscard]] tool_t	Tool() const { return tool; }
	// 0 = no snapping
	void				SetGridSize( float size ) { gridSize = size; }
	[[nodiscard]] float	GridSize() const { return gridSize; }
	// how high the box tool's brush is; 0 = eight steps of the grid
	void				SetBoxHeight( float height ) { boxHeight = height; }
	[[nodiscard]] float	BoxHeight() const;

	// what letting go of a drag asks for: entities were changed already, brushes are moved by the caller
	struct dragResult_t {
		enum class kind_t { NONE, ENTITIES, BRUSHES, FACE };
		kind_t				kind = kind_t::NONE;
		idVec3				delta = vec3_origin;	// BRUSHES
		float				distance = 0.0f;		// FACE: along its normal
	};
	// a press on the gizmo starts a drag: true, and the click is not a selection
	[[nodiscard]] bool	BeginDrag( const idEditorWorld & world, const idVec3 & rayStart, const idVec3 & rayDir, const idVec3 & viewOrigin );
	void				Drag( idEditorWorld & world, const idVec3 & rayStart, const idVec3 & rayDir );
	// ENTITIES: the drag changed the map and a step was added to the history
	[[nodiscard]] dragResult_t	EndDrag( idEditorWorld & world, idEditorHistory & history );
	[[nodiscard]] bool	Dragging() const { return dragging; }

	// The box tool: the rectangle starts where the ray meets what pick found, or the plane z = 0.
	[[nodiscard]] bool	BeginBox( const idEditorWorld & world, const idVec3 & rayStart, const idVec3 & rayDir, const editorPick_t & pick );
	void				DragBox( const idVec3 & rayStart, const idVec3 & rayDir );
	// false: the rectangle has no area on the grid
	[[nodiscard]] bool	EndBox( idBounds & box );
	[[nodiscard]] bool	DrawingBox() const { return boxActive; }

	void				Draw( const idEditorWorld & world, const idVec3 & viewOrigin );

private:
	// where the gizmo is and along what it moves; numAxes 3 = the world's axes, 1 = axes[0] only
	struct gizmo_t {
		idVec3				pivot = vec3_origin;
		idVec3				axes[3];
		int					numAxes = 0;
	};
	[[nodiscard]] bool	Gizmo( const idEditorWorld & world, gizmo_t & gizmo ) const;
	// the point of the dragged handle the ray is at: along the axis, or the angle around it
	[[nodiscard]] bool	HandleValue( const idVec3 & rayStart, const idVec3 & rayDir, float & value ) const;
	[[nodiscard]] idBounds	Box() const;

	idList<int>			selection;
	idList<brushRef_t>	brushes;
	int					face = -1;
	mode_t				mode = mode_t::MOVE;
	tool_t				tool = tool_t::SELECT;
	float				gridSize = 8.0f;
	float				boxHeight = 0.0f;

	bool				dragging = false;
	int					dragAxis = -1;			// of dragGizmo.axes
	gizmo_t				dragGizmo;
	float				dragStartValue = 0.0f;
	float				dragSize = 1.0f;		// the gizmo's size when the drag began
	float				dragDelta = 0.0f;		// along the axis, snapped
	idList<idDict>		dragBefore;				// the map args of the selection, in its order
	bool				dragChanged = false;

	bool				boxActive = false;
	int					boxAxis = 2;			// the rectangle's plane is across this axis
	float				boxSide = 1.0f;			// the brush rises to this side of the plane
	idVec3				boxStart = vec3_origin;
	idVec3				boxEnd = vec3_origin;
};
