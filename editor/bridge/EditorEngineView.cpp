// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include "EditorEngineLocal.h"

namespace {

constexpr char		EDITOR_MINS_KEY[] = "editor_mins";

// An entityDef made of brushes says "editor_mins" "?". Read from the text: parsing a def caches its media.
[[nodiscard]] bool IsBrushClass( const idDecl * decl ) {
	const int length = decl->GetTextLength();
	if ( length <= 0 ) {
		return false;
	}
	idList<char> text;
	text.SetNum( length + 1 );
	decl->GetText( text.Ptr() );
	text[length] = '\0';
	const int at = idStr::FindText( text.Ptr(), EDITOR_MINS_KEY, false );
	if ( at == -1 ) {
		return false;
	}
	const char * value = text.Ptr() + at + sizeof( EDITOR_MINS_KEY ) - 1;
	while ( *value == '"' || *value == ' ' || *value == '\t' ) {
		value++;
	}
	return *value == '?';
}

}	// namespace

void idEditorEngineLocal::SetTool( int newTool ) {
	if ( tool.Dragging() ) {
		return;
	}
	tool.SetTool( ( newTool == EDITOR_TOOL_BOX ) ? idEditorTool::tool_t::BOX : idEditorTool::tool_t::SELECT );
	if ( started ) {
		ui->OnDocumentChanged();
	}
}

void idEditorEngineLocal::SetBoxHeight( float height ) {
	tool.SetBoxHeight( Max( 0.0f, height ) );
	if ( started ) {
		ui->OnDocumentChanged();
	}
}

void idEditorEngineLocal::MoveSelection( const float delta[3] ) {
	if ( !CanEdit() ) {
		return;
	}
	if ( tool.NumSelectedBrushes() > 0 ) {
		MoveBrushes( delta );
		return;
	}
	const idVec3 move( Snap( delta[0] ), Snap( delta[1] ), Snap( delta[2] ) );
	if ( move == vec3_origin ) {
		return;
	}
	idList<editorChange_t> changes;
	for ( int i = 0; i < tool.NumSelected(); i++ ) {
		const int index = tool.Selected( i );
		// a brush entity without an origin is where its brushes are
		if ( !IsEntity( index ) || world.MapArgs( index ).FindKey( "origin" ) == nullptr ) {
			continue;
		}
		editorChange_t & change = changes.Alloc();
		change.index = index;
		change.before = world.MapArgs( index );
		change.after = change.before;
		change.after.SetVector( "origin", change.before.GetVector( "origin" ) + move );
	}
	Edit( "Move", changes );
}

void idEditorEngineLocal::EnumBrushEntityClasses( editorClassVisitor_t visitor, void * context ) {
	if ( !started ) {
		return;
	}
	if ( brushClasses.Num() == 0 ) {
		const int num = declManager->GetNumDecls( DECL_ENTITYDEF );
		for ( int i = 0; i < num; i++ ) {
			const idDecl * const decl = declManager->DeclByIndex( DECL_ENTITYDEF, i, false );
			if ( idStr::Icmp( decl->GetName(), "func_static" ) != 0 && idStr::Icmp( decl->GetName(), "worldspawn" ) != 0 && IsBrushClass( decl ) ) {
				brushClasses.Append( decl->GetName() );
			}
		}
		std::sort( brushClasses.Ptr(), brushClasses.Ptr() + brushClasses.Num(), []( const idStr & a, const idStr & b ) { return a.Icmp( b ) < 0; } );
		brushClasses.Insert( "func_static", 0 );
	}
	for ( int i = 0; i < brushClasses.Num(); i++ ) {
		visitor( context, brushClasses[i] );
	}
}

bool idEditorEngineLocal::EnumViewBrushes( const float mins[3], const float maxs[3], int maxBrushes, editorViewBrushVisitor_t visitor, void * context ) {
	if ( !world.IsOpen() ) {
		return true;
	}
	const idBounds box( idVec3( mins[0], mins[1], mins[2] ), idVec3( maxs[0], maxs[1], maxs[2] ) );
	idList<float> edges;
	int visited = 0;
	for ( int i = 0; i < world.NumEntities(); i++ ) {
		const editorMapEntity_t & entity = world.Entity( i );
		if ( entity.removed || world.NumPrimitives( i ) == 0 ) {
			continue;
		}
		const idList<idBounds> & bounds = world.PrimitiveBounds( i );
		const bool entitySelected = tool.IsSelected( i );
		for ( int j = 0; j < bounds.Num(); j++ ) {
			if ( bounds[j].IsCleared() ) {
				continue;
			}
			idBounds inWorld;
			inWorld.FromTransformedBounds( bounds[j], entity.origin, entity.axis );
			if ( !inWorld.IntersectsBounds( box ) ) {
				continue;
			}
			if ( visited++ >= maxBrushes ) {
				return false;
			}
			editorViewBrush_t info = {};
			info.entity = i;
			info.primitive = j;
			info.kind = EDITOR_VIEW_PATCH;
			info.selected = entitySelected || tool.IsBrushSelected( brushRef_t{ i, j } );
			for ( int axis = 0; axis < 3; axis++ ) {
				info.mins[axis] = inWorld[0][axis];
				info.maxs[axis] = inWorld[1][axis];
			}
			edges.SetNum( 0 );
			editorBrushWindings_t windings;
			if ( const idMapBrush * const brush = world.Brush( i, j ); brush != nullptr && world.BrushWindings( i, j, windings ) ) {
				// the kinds of the two enums are in the same order
				info.kind = static_cast<int>( editorBrush::Kind( *brush ) );
				for ( int side = 0; side < windings.Num(); side++ ) {
					const idFixedWinding & winding = windings[side];
					for ( int k = 0; k < winding.GetNumPoints(); k++ ) {
						const idVec3 & a = winding[k].ToVec3();
						const idVec3 & b = winding[( k + 1 ) % winding.GetNumPoints()].ToVec3();
						edges.Append( a.x );
						edges.Append( a.y );
						edges.Append( a.z );
						edges.Append( b.x );
						edges.Append( b.y );
						edges.Append( b.z );
					}
				}
			}
			info.numEdges = edges.Num() / 6;
			info.edges = edges.Ptr();
			visitor( context, info );
		}
	}
	return true;
}

void idEditorEngineLocal::EnumViewEntities( const float mins[3], const float maxs[3], editorViewEntityVisitor_t visitor, void * context ) {
	if ( !world.IsOpen() ) {
		return;
	}
	const idBounds box( idVec3( mins[0], mins[1], mins[2] ), idVec3( maxs[0], maxs[1], maxs[2] ) );
	for ( int i = 1; i < world.NumEntities(); i++ ) {
		const editorMapEntity_t & entity = world.Entity( i );
		if ( entity.removed ) {
			continue;
		}
		const bool hasPrimitives = world.HasPrimitives( i );
		// a brush entity is where its brushes are, whatever is shown of it
		const idBounds bounds = hasPrimitives ? idBounds( entity.origin ) : world.WorldBounds( i );
		if ( !hasPrimitives && !bounds.IntersectsBounds( box ) ) {
			continue;
		}
		editorViewEntity_t info = {};
		info.index = i;
		info.isLight = entity.isLight;
		info.selected = tool.IsSelected( i );
		info.hasPrimitives = hasPrimitives;
		for ( int axis = 0; axis < 3; axis++ ) {
			info.origin[axis] = entity.origin[axis];
			info.mins[axis] = bounds[0][axis];
			info.maxs[axis] = bounds[1][axis];
		}
		visitor( context, info );
	}
}

void idEditorEngineLocal::GetCamera( editorCamera_t & camera ) const {
	const idVec3 forward = view.Axis()[0];
	for ( int axis = 0; axis < 3; axis++ ) {
		camera.origin[axis] = view.Origin()[axis];
		camera.forward[axis] = forward[axis];
	}
}

void idEditorEngineLocal::GoToLeak() {
	if ( !world.IsOpen() || playing || world.NumLeakPoints() < 2 ) {
		return;
	}
	idVec3 along = world.LeakPoint( 1 ) - world.LeakPoint( 0 );
	if ( along.Normalize() < 1.0e-3f ) {
		along.Set( 1.0f, 0.0f, 0.0f );
	}
	view.SetView( world.LeakPoint( 0 ), along.ToAngles() );
	world.ShowLeak( true );
	ui->OnDocumentChanged();
}

CONSOLE_COMMAND_SHIP( editorGoToLeak, "puts the edit view's camera at the start of the leak line", nullptr ) {
	if ( Editor_NeedEngine() ) {
		editorEngine->GoToLeak();
	}
}
