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
	  drag a ring        turn around that axis, in steps of the angle snap
	  box tool: drag     a rectangle on the face or the grid plane under the cursor; letting go
	                     makes a brush of it, as high as the box height

	The gizmo sits at the first selected entity, in the middle of the selected brushes and patches
	(for a turn: that middle on the grid), or as one arrow along its normal on the selected face.
	A drag changes entities live; of brushes, patches and a face it shows the outline where they
	will be. Letting go makes it one step of the history.

	The grid is drawn on the working plane: z = 0, or the plane the box tool drew on last.
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

	// Brushes and patches are selected instead of entities, never along with them: a brushRef_t is
	// a primitive of either kind. The face is a side of the one selected brush, or -1.
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
	void				SetTool( tool_t newTool );
	[[nodiscard]] tool_t	Tool() const { return tool; }
	// 0 = no snapping
	void				SetGridSize( float size ) { gridSize = size; }
	[[nodiscard]] float	GridSize() const { return gridSize; }
	// how high the box tool's brush is; 0 = eight steps of the grid
	void				SetBoxHeight( float height ) { boxHeight = height; }
	[[nodiscard]] float	BoxHeight() const;
	// degrees a turn snaps to; 0 = none
	void				SetAngleSnap( float degrees ) { angleSnap = degrees; }
	[[nodiscard]] float	AngleSnap() const { return angleSnap; }
	void				ShowGrid( bool show ) { showGrid = show; }
	[[nodiscard]] bool	GridShown() const { return showGrid; }
	// The point brushes and patches turn around: the middle of the selection, on the grid.
	[[nodiscard]] bool	RotatePivot( const idEditorWorld & world, idVec3 & pivot ) const;

	// what letting go of a drag asks for: entities were changed already, brushes are moved by the caller
	struct dragResult_t {
		enum class kind_t { NONE, ENTITIES, BRUSHES, FACE, ROTATE };
		kind_t				kind = kind_t::NONE;
		idVec3				delta = vec3_origin;	// BRUSHES
		float				distance = 0.0f;		// FACE: along its normal; ROTATE: degrees
		int					axis = 0;				// ROTATE: 0..2
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
	// the gizmo is arrows: always for a face, else in move mode
	[[nodiscard]] bool	Moves() const { return mode == mode_t::MOVE || SelectedFace() != -1; }
	// the edges of a selected brush or the control net of a selected patch, as pairs of points in the world
	void				Outline( const idEditorWorld & world, const brushRef_t & primitive, int side, idList<idVec3> & points ) const;
	void				DrawGrid( idRenderWorld * renderWorld, const idVec3 & viewOrigin ) const;

	idList<int>			selection;
	idList<brushRef_t>	brushes;
	int					face = -1;
	mode_t				mode = mode_t::MOVE;
	tool_t				tool = tool_t::SELECT;
	float				gridSize = 8.0f;
	float				boxHeight = 0.0f;
	float				angleSnap = 15.0f;
	bool				showGrid = true;
	int					planeAxis = 2;			// the working plane is across this axis
	float				planeDist = 0.0f;

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
