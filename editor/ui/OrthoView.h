// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QLineF>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QTimer>
#include <QWidget>

class idEditorEngine;

/*
	A 2D view of the map, drawn by Qt from what the engine enumerates: the outlines of the brushes,
	the boxes of the entities, the selection, the grid and the camera.

	  Top    x to the right, y up        Front   x to the right, z up        Side   y to the right, z up

	  middle button drag    pan                 wheel            zoom at the cursor
	  click                 select (Ctrl adds or removes); a brush entity as a whole
	  drag the selection    move it on the grid, one step of the history
	  box tool: drag        a rectangle that becomes a brush; its third size is that of the brush
	                        selected last, or the box height from 0

	The outlines are asked for again when the map or the selection changes and when the view leaves
	the part it has; never more than MAX_BRUSHES of them, the rest needs a closer look.
*/
class OrthoView final : public QWidget {
	Q_OBJECT

public:
	enum Kind { KIND_TOP, KIND_FRONT, KIND_SIDE };

	explicit OrthoView( idEditorEngine * engine, QWidget * parent = nullptr );

	void				SetKind( int kind );
	// the map, the selection or the tool changed
	void				Reload();
	// the middle of the view goes to the camera
	void				CenterOnCamera();

signals:
	void				statusText( const QString & text );

protected:
	void				paintEvent( QPaintEvent * event ) override;
	void				mousePressEvent( QMouseEvent * event ) override;
	void				mouseMoveEvent( QMouseEvent * event ) override;
	void				mouseReleaseEvent( QMouseEvent * event ) override;
	void				wheelEvent( QWheelEvent * event ) override;

private:
	struct Brush {
		int					entity;
		int					primitive;
		int					kind;
		bool				selected;
		QRectF				box;			// in the view's plane
		float				depth[2];		// across it
		QList<QLineF>		lines;
	};
	struct Entity {
		int					index;
		bool				isLight;
		bool				selected;
		QRectF				box;
	};
	enum class Drag { NONE, PAN, CLICK, MOVE, BOX };

	void				Query();
	void				PollCamera();
	[[nodiscard]] QPointF	ToScreen( const QPointF & world ) const;
	[[nodiscard]] QPointF	ToWorld( const QPointF & screen ) const;
	[[nodiscard]] QRectF	VisibleWorld() const;
	[[nodiscard]] QPointF	Snapped( const QPointF & world ) const;
	[[nodiscard]] double	GridStep() const { return ( gridSize > 0.0f ) ? gridSize : 1.0; }
	// what a click at the point selects; false: nothing there
	[[nodiscard]] bool	HitTest( const QPointF & world, bool & isEntity, int & entity, int & primitive, bool & selected, bool & patch ) const;
	void				Click( const QPointF & world, bool ctrl );

	idEditorEngine *	engine;
	int					axisU = 0;
	int					axisV = 1;
	int					axisDepth = 2;
	QPointF				center = QPointF( 0.0, 0.0 );	// the world point in the middle
	double				scale = 0.5;					// pixels per unit

	QList<Brush>		brushes;
	QList<Entity>		entities;
	QRectF				have;							// the part of the plane the lists cover
	bool				valid = false;
	bool				queryPending = false;
	bool				truncated = false;
	bool				mapOpen = false;
	bool				playing = false;
	bool				boxTool = false;
	float				gridSize = 8.0f;
	float				boxHeight = 64.0f;
	float				lastDepth[2] = { 0.0f, 0.0f };	// of the brush selected last; equal = none yet

	QTimer				cameraTimer;
	QPointF				camera;
	QPointF				cameraForward;

	Drag				drag = Drag::NONE;
	QPointF				pressScreen;
	QPointF				pressWorld;
	QPointF				dragWorld;
	bool				pressCtrl = false;
};
