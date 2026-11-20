// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "OrthoView.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <cmath>

#include "../api/EditorEngineApi.h"

namespace {

constexpr int		MAX_BRUSHES = 4000;
constexpr float		DEPTH_ALL = 1.0e6f;				// the view shows every depth
constexpr double	QUERY_MARGIN = 0.5;				// of the view's size, asked for around it
constexpr double	SCALE_MIN = 0.005;
constexpr double	SCALE_MAX = 64.0;
constexpr double	ZOOM_PER_NOTCH = 1.2;
constexpr double	MIN_GRID_PIXELS = 6.0;
constexpr int		GRID_MAJOR = 8;					// every this many lines is a brighter one
constexpr int		DRAG_START_PIXELS = 4;
constexpr double	CAMERA_SIZE = 10.0;				// pixels
constexpr int		CAMERA_POLL_MSEC = 100;

const QColor		BACKGROUND( 24, 24, 26 );
const QColor		GRID_MINOR( 40, 40, 44 );
const QColor		GRID_MAJOR_COLOR( 58, 58, 64 );
const QColor		GRID_AXIS( 90, 90, 100 );
const QColor		SELECTED( 255, 190, 30 );
const QColor		GHOST( 255, 255, 255 );
const QColor		BOX( 80, 255, 255 );
const QColor		CAMERA_COLOR( 120, 220, 120 );

[[nodiscard]] QColor BrushColor( int kind, bool ofEntity ) {
	switch ( kind ) {
		case EDITOR_VIEW_BRUSH_HIDDEN:	return QColor( 90, 90, 90 );
		case EDITOR_VIEW_BRUSH_PORTAL:	return QColor( 255, 60, 255 );
		case EDITOR_VIEW_BRUSH_CLIP:	return QColor( 230, 230, 60 );
		case EDITOR_VIEW_BRUSH_TRIGGER:	return QColor( 255, 140, 40 );
		case EDITOR_VIEW_PATCH:			return QColor( 70, 170, 170 );
		default:						return ofEntity ? QColor( 110, 160, 230 ) : QColor( 190, 190, 190 );
	}
}

}	// namespace

OrthoView::OrthoView( idEditorEngine * engine_, QWidget * parent ) : QWidget( parent ), engine( engine_ ) {
	setMinimumSize( 160, 120 );
	setMouseTracking( false );
	setFocusPolicy( Qt::ClickFocus );
	cameraTimer.setInterval( CAMERA_POLL_MSEC );
	connect( &cameraTimer, &QTimer::timeout, this, &OrthoView::PollCamera );
	cameraTimer.start();
}

void OrthoView::SetKind( int kind ) {
	const int axes[3][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 2, 0 } };
	const int row = qBound( 0, kind, 2 );
	editorCamera_t now = {};
	engine->GetCamera( now );
	axisU = axes[row][0];
	axisV = axes[row][1];
	axisDepth = axes[row][2];
	center = QPointF( now.origin[axisU], now.origin[axisV] );
	Reload();
}

void OrthoView::Reload() {
	valid = false;
	update();
	// not only in a paint: a view behind another window is not painted, and its numbers are read
	if ( !queryPending ) {
		queryPending = true;
		QTimer::singleShot( 0, this, [this] {
			queryPending = false;
			if ( !valid && isVisible() ) {
				Query();
			}
		} );
	}
}

void OrthoView::CenterOnCamera() {
	editorCamera_t now = {};
	engine->GetCamera( now );
	center = QPointF( now.origin[axisU], now.origin[axisV] );
	update();
}

void OrthoView::PollCamera() {
	if ( !isVisible() ) {
		return;
	}
	editorCamera_t now = {};
	engine->GetCamera( now );
	const QPointF origin( now.origin[axisU], now.origin[axisV] );
	const QPointF forward( now.forward[axisU], now.forward[axisV] );
	if ( origin != camera || forward != cameraForward ) {
		camera = origin;
		cameraForward = forward;
		update();
	}
}

QPointF OrthoView::ToScreen( const QPointF & world ) const {
	return QPointF( width() * 0.5 + ( world.x() - center.x() ) * scale, height() * 0.5 - ( world.y() - center.y() ) * scale );
}

QPointF OrthoView::ToWorld( const QPointF & screen ) const {
	return QPointF( center.x() + ( screen.x() - width() * 0.5 ) / scale, center.y() - ( screen.y() - height() * 0.5 ) / scale );
}

QRectF OrthoView::VisibleWorld() const {
	const double halfWidth = width() * 0.5 / scale;
	const double halfHeight = height() * 0.5 / scale;
	return QRectF( center.x() - halfWidth, center.y() - halfHeight, halfWidth * 2.0, halfHeight * 2.0 );
}

QPointF OrthoView::Snapped( const QPointF & world ) const {
	const double step = GridStep();
	return QPointF( std::round( world.x() / step ) * step, std::round( world.y() / step ) * step );
}

void OrthoView::Query() {
	editorDocumentState_t state = {};
	engine->GetDocumentState( state );
	mapOpen = engine->CurrentMap()[0] != '\0';
	playing = state.playing;
	boxTool = ( state.tool == EDITOR_TOOL_BOX );
	gridSize = state.gridSize;
	boxHeight = state.boxHeight;

	const QRectF visible = VisibleWorld();
	have = visible.adjusted( -visible.width() * QUERY_MARGIN, -visible.height() * QUERY_MARGIN, visible.width() * QUERY_MARGIN, visible.height() * QUERY_MARGIN );
	float mins[3];
	float maxs[3];
	mins[axisU] = static_cast<float>( have.left() );
	maxs[axisU] = static_cast<float>( have.right() );
	mins[axisV] = static_cast<float>( have.top() );
	maxs[axisV] = static_cast<float>( have.bottom() );
	mins[axisDepth] = -DEPTH_ALL;
	maxs[axisDepth] = DEPTH_ALL;

	brushes.clear();
	entities.clear();
	truncated = false;
	valid = true;
	if ( !mapOpen || playing ) {
		setProperty( "numBrushes", 0 );
		setProperty( "numSelected", 0 );
		return;
	}
	truncated = !engine->EnumViewBrushes( mins, maxs, MAX_BRUSHES, []( void * context, const editorViewBrush_t & info ) {
		auto * const self = static_cast<OrthoView *>( context );
		Brush brush;
		brush.entity = info.entity;
		brush.primitive = info.primitive;
		brush.kind = info.kind;
		brush.selected = info.selected;
		brush.box = QRectF( QPointF( info.mins[self->axisU], info.mins[self->axisV] ), QPointF( info.maxs[self->axisU], info.maxs[self->axisV] ) );
		brush.depth[0] = info.mins[self->axisDepth];
		brush.depth[1] = info.maxs[self->axisDepth];
		brush.lines.reserve( info.numEdges );
		for ( int i = 0; i < info.numEdges; i++ ) {
			const float * const edge = info.edges + i * 6;
			const QLineF line( edge[self->axisU], edge[self->axisV], edge[3 + self->axisU], edge[3 + self->axisV] );
			// an edge seen from its end is a point
			if ( line.p1() != line.p2() ) {
				brush.lines.append( line );
			}
		}
		if ( brush.selected && info.entity == 0 ) {
			self->lastDepth[0] = brush.depth[0];
			self->lastDepth[1] = brush.depth[1];
		}
		self->brushes.append( brush );
	}, this );
	engine->EnumViewEntities( mins, maxs, []( void * context, const editorViewEntity_t & info ) {
		auto * const self = static_cast<OrthoView *>( context );
		if ( info.hasPrimitives ) {
			return;
		}
		Entity entity;
		entity.index = info.index;
		entity.isLight = info.isLight;
		entity.selected = info.selected;
		entity.box = QRectF( QPointF( info.mins[self->axisU], info.mins[self->axisV] ), QPointF( info.maxs[self->axisU], info.maxs[self->axisV] ) );
		self->entities.append( entity );
	}, this );

	// for the UI script
	int selected = 0;
	for ( const Brush & brush : brushes ) {
		selected += brush.selected ? 1 : 0;
	}
	for ( const Entity & entity : entities ) {
		selected += entity.selected ? 1 : 0;
	}
	setProperty( "numBrushes", static_cast<int>( brushes.size() ) );
	setProperty( "numSelected", selected );
}

void OrthoView::paintEvent( QPaintEvent * ) {
	if ( !valid || !have.contains( VisibleWorld() ) ) {
		Query();
	}
	QPainter painter( this );
	painter.fillRect( rect(), BACKGROUND );
	if ( !mapOpen || playing ) {
		painter.setPen( GRID_AXIS );
		painter.drawText( rect(), Qt::AlignCenter, playing ? tr( "The game plays the map" ) : tr( "No map is open" ) );
		return;
	}

	// the grid, in steps the eye can tell apart
	double step = GridStep();
	while ( step * scale < MIN_GRID_PIXELS ) {
		step *= 2.0;
	}
	const QRectF visible = VisibleWorld();
	for ( int pass = 0; pass < 2; pass++ ) {
		const double first = std::floor( ( ( pass == 0 ) ? visible.left() : visible.top() ) / step );
		const double last = std::ceil( ( ( pass == 0 ) ? visible.right() : visible.bottom() ) / step );
		for ( double i = first; i <= last; i += 1.0 ) {
			const qint64 index = static_cast<qint64>( i );
			painter.setPen( ( index == 0 ) ? GRID_AXIS : ( ( index % GRID_MAJOR == 0 ) ? GRID_MAJOR_COLOR : GRID_MINOR ) );
			if ( pass == 0 ) {
				const double x = ToScreen( QPointF( i * step, 0.0 ) ).x();
				painter.drawLine( QPointF( x, 0.0 ), QPointF( x, height() ) );
			} else {
				const double y = ToScreen( QPointF( 0.0, i * step ) ).y();
				painter.drawLine( QPointF( 0.0, y ), QPointF( width(), y ) );
			}
		}
	}

	const auto screenRect = [this]( const QRectF & box ) {
		return QRectF( ToScreen( box.topLeft() ), ToScreen( box.bottomRight() ) ).normalized();
	};
	const QPointF moved = ( drag == Drag::MOVE ) ? Snapped( dragWorld - pressWorld ) : QPointF();

	// the selection last, over the rest
	for ( int pass = 0; pass < 2; pass++ ) {
		for ( const Brush & brush : brushes ) {
			// a flat box is not an empty one here, so no QRectF::intersects
			if ( brush.selected != ( pass == 1 ) || brush.box.left() > visible.right() || brush.box.right() < visible.left()
				|| brush.box.top() > visible.bottom() || brush.box.bottom() < visible.top() ) {
				continue;
			}
			painter.setPen( QPen( brush.selected ? SELECTED : BrushColor( brush.kind, brush.entity != 0 ), brush.selected ? 2.0 : 1.0,
				( brush.kind == EDITOR_VIEW_PATCH ) ? Qt::DashLine : Qt::SolidLine ) );
			if ( brush.kind == EDITOR_VIEW_PATCH ) {
				painter.drawRect( screenRect( brush.box ) );
			}
			for ( const QLineF & line : brush.lines ) {
				painter.drawLine( ToScreen( line.p1() ), ToScreen( line.p2() ) );
			}
			if ( brush.selected && drag == Drag::MOVE && !moved.isNull() ) {
				painter.setPen( GHOST );
				for ( const QLineF & line : brush.lines ) {
					painter.drawLine( ToScreen( line.p1() + moved ), ToScreen( line.p2() + moved ) );
				}
			}
		}
		for ( const Entity & entity : entities ) {
			if ( entity.selected != ( pass == 1 ) ) {
				continue;
			}
			painter.setPen( QPen( entity.selected ? SELECTED : ( entity.isLight ? QColor( 240, 230, 140 ) : QColor( 80, 150, 255 ) ), entity.selected ? 2.0 : 1.0 ) );
			painter.drawRect( screenRect( entity.box ) );
			if ( entity.selected && drag == Drag::MOVE && !moved.isNull() ) {
				painter.setPen( GHOST );
				painter.drawRect( screenRect( entity.box.translated( moved ) ) );
			}
		}
	}

	if ( drag == Drag::BOX ) {
		painter.setPen( QPen( BOX, 2.0 ) );
		painter.drawRect( screenRect( QRectF( Snapped( pressWorld ), Snapped( dragWorld ) ) ) );
	}

	// the camera: where it is and where it looks
	const QPointF at = ToScreen( camera );
	QPointF forward( cameraForward.x(), -cameraForward.y() );
	const double length = std::hypot( forward.x(), forward.y() );
	painter.setPen( QPen( CAMERA_COLOR, 2.0 ) );
	painter.drawEllipse( at, CAMERA_SIZE * 0.4, CAMERA_SIZE * 0.4 );
	if ( length > 0.05 ) {
		forward /= length;
		painter.drawLine( at, at + forward * CAMERA_SIZE * 2.0 );
	}

	if ( truncated ) {
		painter.setPen( SELECTED );
		painter.drawText( rect().adjusted( 6, 4, -6, -4 ), Qt::AlignTop | Qt::AlignLeft, tr( "More than %1 brushes here: zoom in to see all of them" ).arg( MAX_BRUSHES ) );
	}
}

bool OrthoView::HitTest( const QPointF & world, bool & isEntity, int & entity, int & primitive, bool & selected, bool & patch ) const {
	// the smallest box the point is in: what is inside a room is reached before the room
	double best = -1.0;
	const double slack = 3.0 / scale;
	for ( const Entity & candidate : entities ) {
		const QRectF box = candidate.box.normalized().adjusted( -slack, -slack, slack, slack );
		const double area = box.width() * box.height();
		if ( box.contains( world ) && ( best < 0.0 || area < best ) ) {
			best = area;
			isEntity = true;
			entity = candidate.index;
			primitive = -1;
			selected = candidate.selected;
			patch = false;
		}
	}
	for ( const Brush & candidate : brushes ) {
		const QRectF box = candidate.box.normalized().adjusted( -slack, -slack, slack, slack );
		const double area = box.width() * box.height();
		if ( box.contains( world ) && ( best < 0.0 || area < best ) ) {
			best = area;
			isEntity = ( candidate.entity != 0 );
			entity = candidate.entity;
			primitive = candidate.primitive;
			selected = candidate.selected;
			patch = ( candidate.kind == EDITOR_VIEW_PATCH );
		}
	}
	return best >= 0.0;
}

void OrthoView::Click( const QPointF & world, bool ctrl ) {
	bool isEntity = false;
	bool selected = false;
	bool patch = false;
	int entity = 0;
	int primitive = -1;
	if ( !HitTest( world, isEntity, entity, primitive, selected, patch ) ) {
		if ( !ctrl ) {
			engine->SelectEntity( 0, false );
		}
		return;
	}
	if ( isEntity ) {
		engine->SelectEntity( entity, ctrl );
	} else if ( patch ) {
		emit statusText( tr( "A patch is shown only: it cannot be selected or edited yet." ) );
	} else {
		engine->SelectBrush( entity, primitive, -1, ctrl );
	}
}

void OrthoView::mousePressEvent( QMouseEvent * event ) {
	if ( drag != Drag::NONE || !mapOpen || playing ) {
		return;
	}
	pressScreen = event->position();
	pressWorld = ToWorld( pressScreen );
	dragWorld = pressWorld;
	pressCtrl = ( event->modifiers() & Qt::ControlModifier ) != 0;
	if ( event->button() == Qt::MiddleButton ) {
		drag = Drag::PAN;
	} else if ( event->button() == Qt::LeftButton ) {
		drag = boxTool ? Drag::BOX : Drag::CLICK;
	}
	update();
}

void OrthoView::mouseMoveEvent( QMouseEvent * event ) {
	const QPointF now = event->position();
	switch ( drag ) {
		case Drag::PAN:
			// the world point under the cursor stays under it
			center -= QPointF( ( now.x() - pressScreen.x() ) / scale, -( now.y() - pressScreen.y() ) / scale );
			pressScreen = now;
			break;
		case Drag::CLICK: {
			if ( ( now - pressScreen ).manhattanLength() < DRAG_START_PIXELS ) {
				return;
			}
			// a drag that starts on the selection moves it
			bool isEntity = false;
			bool selected = false;
			bool patch = false;
			int entity = 0;
			int primitive = -1;
			bool onSelection = false;
			for ( const Brush & brush : brushes ) {
				onSelection |= ( brush.selected && brush.box.normalized().contains( pressWorld ) );
			}
			for ( const Entity & candidate : entities ) {
				onSelection |= ( candidate.selected && candidate.box.normalized().contains( pressWorld ) );
			}
			if ( !onSelection && !( HitTest( pressWorld, isEntity, entity, primitive, selected, patch ) && selected ) ) {
				return;
			}
			drag = Drag::MOVE;
			dragWorld = ToWorld( now );
			break;
		}
		case Drag::MOVE:
		case Drag::BOX:
			dragWorld = ToWorld( now );
			break;
		default:
			return;
	}
	update();
}

void OrthoView::mouseReleaseEvent( QMouseEvent * event ) {
	const Drag ended = drag;
	if ( ended == Drag::NONE || ( ended == Drag::PAN ) != ( event->button() == Qt::MiddleButton ) ) {
		return;
	}
	drag = Drag::NONE;
	dragWorld = ToWorld( event->position() );
	if ( ended == Drag::CLICK ) {
		Click( pressWorld, pressCtrl );
	} else if ( ended == Drag::MOVE ) {
		const QPointF moved = Snapped( dragWorld - pressWorld );
		if ( !moved.isNull() ) {
			float delta[3] = {};
			delta[axisU] = static_cast<float>( moved.x() );
			delta[axisV] = static_cast<float>( moved.y() );
			engine->MoveSelection( delta );
		}
	} else if ( ended == Drag::BOX ) {
		const QPointF a = Snapped( pressWorld );
		const QPointF b = Snapped( dragWorld );
		if ( a.x() != b.x() && a.y() != b.y() ) {
			float mins[3];
			float maxs[3];
			mins[axisU] = static_cast<float>( qMin( a.x(), b.x() ) );
			maxs[axisU] = static_cast<float>( qMax( a.x(), b.x() ) );
			mins[axisV] = static_cast<float>( qMin( a.y(), b.y() ) );
			maxs[axisV] = static_cast<float>( qMax( a.y(), b.y() ) );
			const bool haveDepth = ( lastDepth[1] > lastDepth[0] );
			mins[axisDepth] = haveDepth ? lastDepth[0] : 0.0f;
			maxs[axisDepth] = haveDepth ? lastDepth[1] : boxHeight;
			engine->CreateBrush( mins, maxs, nullptr );
		}
	}
	update();
}

void OrthoView::wheelEvent( QWheelEvent * event ) {
	const QPointF cursor = event->position();
	const QPointF before = ToWorld( cursor );
	scale = qBound( SCALE_MIN, scale * std::pow( ZOOM_PER_NOTCH, event->angleDelta().y() / 120.0 ), SCALE_MAX );
	// the world point under the cursor stays under it
	const QPointF after = ToWorld( cursor );
	center += before - after;
	update();
}
