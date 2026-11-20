// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include <psapi.h>

#include "EditorEngineLocal.h"

namespace {

// present in the retail game; the test maps of base/maps/editor use it too
constexpr char		DEFAULT_MATERIAL[] = "textures/base_wall/lfwall13f3";
constexpr float		DEFAULT_REPEAT = 64.0f;				// world units per repeat of a new brush's texture
constexpr float		DUPLICATE_OFFSET = 16.0f;
constexpr float		PICK_DISTANCE = 16384.0f;

using primitivePtr_t = std::shared_ptr<const idMapPrimitive>;

[[nodiscard]] editorPrimitiveChange_t Insert( int entity, int primitive, idMapPrimitive * added ) {
	editorPrimitiveChange_t change;
	change.entity = entity;
	change.primitive = primitive;
	change.after = primitivePtr_t( added );
	return change;
}

[[nodiscard]] editorPrimitiveChange_t Remove( const idEditorWorld & world, int entity, int primitive ) {
	editorPrimitiveChange_t change;
	change.entity = entity;
	change.primitive = primitive;
	const idMapPrimitive * const removed = world.Primitive( entity, primitive );
	if ( removed->GetType() == idMapPrimitive::TYPE_BRUSH ) {
		change.before = primitivePtr_t( static_cast<const idMapBrush *>( removed )->Clone() );
	} else {
		change.before = primitivePtr_t( static_cast<const idMapPatch *>( removed )->Clone() );
	}
	return change;
}

[[nodiscard]] editorPrimitiveChange_t Replace( const idEditorWorld & world, int entity, int primitive, idMapBrush * edited ) {
	editorPrimitiveChange_t change = Remove( world, entity, primitive );
	change.after = primitivePtr_t( edited );
	return change;
}

[[nodiscard]] bool IsRotated( const editorMapEntity_t & entity ) {
	return !entity.axis.Compare( mat3_identity, 1.0e-4f );
}

}	// namespace

void idEditorEngineLocal::SelectBrushesOnly( const idList<brushRef_t> & brushes ) {
	tool.ClearSelection();
	for ( int i = 0; i < brushes.Num(); i++ ) {
		tool.SelectBrush( brushes[i], -1, brushes.Num() > 1 );
	}
	ui->OnSelectionChanged();
}

bool idEditorEngineLocal::BrushesToEdit( idList<brushRef_t> & brushes ) const {
	brushes.Clear();
	for ( int i = 0; i < tool.NumSelectedBrushes(); i++ ) {
		const brushRef_t & brush = tool.SelectedBrush( i );
		if ( world.Brush( brush.entity, brush.primitive ) != nullptr ) {
			brushes.Append( brush );
		}
	}
	if ( brushes.Num() == 0 ) {
		common->Printf( "No brush is selected.\n" );
		return false;
	}
	std::sort( brushes.Ptr(), brushes.Ptr() + brushes.Num(), []( const brushRef_t & a, const brushRef_t & b ) {
		return ( a.entity != b.entity ) ? a.entity < b.entity : a.primitive > b.primitive;
	} );
	return true;
}

bool idEditorEngineLocal::Pick( int x, int y, editorPickInfo_t & pick ) {
	pick = { -1, -1, -1, 0.0f, { 0.0f, 0.0f, 0.0f } };
	if ( !world.IsOpen() || playing || world.IsSuspended() ) {
		return false;
	}
	const idVec3 dir = view.RayThroughPixel( x, y );
	const editorPick_t found = world.Pick( view.Origin(), dir, PICK_DISTANCE );
	if ( found.entity == -1 ) {
		return false;
	}
	const idVec3 point = view.Origin() + dir * found.distance;
	pick = { found.entity, found.primitive, found.face, found.distance, { point.x, point.y, point.z } };
	return true;
}

void idEditorEngineLocal::SelectBrush( int entityIndex, int primitive, int face, bool add ) {
	const idMapBrush * const brush = world.IsOpen() ? world.Brush( entityIndex, primitive ) : nullptr;
	if ( brush == nullptr || world.Entity( entityIndex ).removed || tool.Dragging() ) {
		return;
	}
	const int side = ( face >= 0 && face < brush->GetNumSides() ) ? face : -1;
	if ( tool.SelectBrush( brushRef_t{ entityIndex, primitive }, side, add ) ) {
		ui->OnSelectionChanged();
	}
}

void idEditorEngineLocal::EnumBrushSelection( editorBrushVisitor_t visitor, void * context ) {
	for ( int i = 0; i < tool.NumSelectedBrushes(); i++ ) {
		visitor( context, tool.SelectedBrush( i ).entity, tool.SelectedBrush( i ).primitive, tool.SelectedFace() );
	}
}

void idEditorEngineLocal::EnumBrushFaces( int entityIndex, int primitive, editorFaceVisitor_t visitor, void * context ) {
	const idMapBrush * const brush = world.IsOpen() ? world.Brush( entityIndex, primitive ) : nullptr;
	if ( brush == nullptr ) {
		return;
	}
	editorBrushWindings_t windings;
	const bool valid = editorBrush::Windings( *brush, windings );
	for ( int i = 0; i < brush->GetNumSides(); i++ ) {
		const idMapBrushSide & side = *brush->GetSide( i );
		editorFaceInfo_t info = {};
		info.face = i;
		info.material = side.GetMaterial();
		info.normal[0] = side.GetPlane()[0];
		info.normal[1] = side.GetPlane()[1];
		info.normal[2] = side.GetPlane()[2];
		info.dist = side.GetPlane().Dist();
		info.visible = valid && windings[i].GetNumPoints() >= 3;
		editorBrush::GetTexture( side, info.texture.shiftS, info.texture.shiftT, info.texture.repeatS, info.texture.repeatT, info.texture.rotate );
		visitor( context, info );
	}
}

void idEditorEngineLocal::CreateBrush( const float mins[3], const float maxs[3], const char * material ) {
	if ( !CanEdit() ) {
		return;
	}
	const char * const materialName = ( material != nullptr && material[0] != '\0' ) ? material : DEFAULT_MATERIAL;
	if ( declManager->FindMaterial( materialName, false ) == nullptr ) {
		common->Printf( "There is no material '%s'.\n", materialName );
		return;
	}
	idBounds bounds;
	for ( int axis = 0; axis < 3; axis++ ) {
		bounds[0][axis] = Snap( Min( mins[axis], maxs[axis] ) );
		bounds[1][axis] = Snap( Max( mins[axis], maxs[axis] ) );
		if ( bounds[1][axis] - bounds[0][axis] <= 0.0f ) {
			common->Printf( "The box has no volume on the grid of %g.\n", tool.GridSize() );
			return;
		}
	}
	// into the brush entity that is selected, or one of whose brushes is
	int entity = 0;
	if ( tool.NumSelected() == 1 && world.HasPrimitives( tool.Selected( 0 ) ) ) {
		entity = tool.Selected( 0 );
	} else if ( tool.NumSelectedBrushes() > 0 ) {
		entity = tool.SelectedBrush( 0 ).entity;
	}
	if ( IsRotated( world.Entity( entity ) ) ) {
		common->Printf( "The entity is rotated: an axial box does not fit it.\n" );
		return;
	}

	idList<editorPrimitiveChange_t> primitiveChanges;
	const int primitive = world.NumPrimitives( entity );
	primitiveChanges.Append( Insert( entity, primitive, editorBrush::MakeBox( bounds, materialName, DEFAULT_REPEAT, world.Entity( entity ).origin ) ) );
	Edit( "Create brush", idList<editorChange_t>(), primitiveChanges );

	idList<brushRef_t> created;
	created.Append( brushRef_t{ entity, primitive } );
	SelectBrushesOnly( created );
}

void idEditorEngineLocal::MoveBrushes( const float delta[3] ) {
	idList<brushRef_t> brushes;
	if ( !CanEdit() || !BrushesToEdit( brushes ) ) {
		return;
	}
	const idVec3 move( Snap( delta[0] ), Snap( delta[1] ), Snap( delta[2] ) );
	if ( move == vec3_origin ) {
		return;
	}
	idList<editorPrimitiveChange_t> primitiveChanges;
	for ( int i = 0; i < brushes.Num(); i++ ) {
		idMapBrush * const moved = world.Brush( brushes[i].entity, brushes[i].primitive )->Clone();
		editorBrush::Translate( *moved, move * world.Entity( brushes[i].entity ).axis.Transpose() );
		primitiveChanges.Append( Replace( world, brushes[i].entity, brushes[i].primitive, moved ) );
	}
	Edit( ( brushes.Num() == 1 ) ? "Move brush" : va( "Move %d brushes", brushes.Num() ), idList<editorChange_t>(), primitiveChanges );
}

void idEditorEngineLocal::MoveFace( float distance ) {
	if ( !CanEdit() ) {
		return;
	}
	if ( tool.NumSelectedBrushes() != 1 || tool.SelectedFace() == -1 ) {
		common->Printf( "Select one face first: editorSelectBrush <entity> <primitive> <face>.\n" );
		return;
	}
	const brushRef_t brush = tool.SelectedBrush( 0 );
	const float move = Snap( distance );
	if ( move == 0.0f ) {
		return;
	}
	idMapBrush * const moved = world.Brush( brush.entity, brush.primitive )->Clone();
	if ( !editorBrush::MoveFace( *moved, tool.SelectedFace(), move ) ) {
		delete moved;
		common->Printf( "The face cannot move by %g: the brush would lose a face or its volume.\n", move );
		return;
	}
	idList<editorPrimitiveChange_t> primitiveChanges;
	primitiveChanges.Append( Replace( world, brush.entity, brush.primitive, moved ) );
	Edit( "Move face", idList<editorChange_t>(), primitiveChanges );
}

void idEditorEngineLocal::DeleteBrushes() {
	idList<brushRef_t> brushes;
	if ( !BrushesToEdit( brushes ) ) {
		return;
	}
	idList<editorPrimitiveChange_t> primitiveChanges;
	for ( int i = 0; i < brushes.Num(); i++ ) {
		primitiveChanges.Append( Remove( world, brushes[i].entity, brushes[i].primitive ) );
	}
	tool.ClearSelection();
	Edit( ( brushes.Num() == 1 ) ? "Delete brush" : va( "Delete %d brushes", brushes.Num() ), idList<editorChange_t>(), primitiveChanges );
	ui->OnSelectionChanged();
}

void idEditorEngineLocal::DuplicateBrushes() {
	idList<brushRef_t> brushes;
	if ( !BrushesToEdit( brushes ) ) {
		return;
	}
	const float step = ( tool.GridSize() > 0.0f ) ? tool.GridSize() * 2.0f : DUPLICATE_OFFSET;
	idList<editorPrimitiveChange_t> primitiveChanges;
	idList<brushRef_t> copies;
	int added = 0;
	for ( int i = 0; i < brushes.Num(); i++ ) {
		const int entity = brushes[i].entity;
		if ( i > 0 && entity != brushes[i - 1].entity ) {
			added = 0;
		}
		idMapBrush * const copy = world.Brush( entity, brushes[i].primitive )->Clone();
		editorBrush::Translate( *copy, idVec3( step, step, 0.0f ) * world.Entity( entity ).axis.Transpose() );
		const int primitive = world.NumPrimitives( entity ) + added++;
		primitiveChanges.Append( Insert( entity, primitive, copy ) );
		copies.Append( brushRef_t{ entity, primitive } );
	}
	Edit( ( brushes.Num() == 1 ) ? "Duplicate brush" : va( "Duplicate %d brushes", brushes.Num() ), idList<editorChange_t>(), primitiveChanges );
	SelectBrushesOnly( copies );
}

void idEditorEngineLocal::SetFaceMaterial( const char * material ) {
	idList<brushRef_t> brushes;
	if ( !CanEdit() || material == nullptr || !BrushesToEdit( brushes ) ) {
		return;
	}
	if ( declManager->FindMaterial( material, false ) == nullptr ) {
		common->Printf( "There is no material '%s'.\n", material );
		return;
	}
	const int face = tool.SelectedFace();
	idList<editorPrimitiveChange_t> primitiveChanges;
	for ( int i = 0; i < brushes.Num(); i++ ) {
		idMapBrush * const edited = world.Brush( brushes[i].entity, brushes[i].primitive )->Clone();
		for ( int side = 0; side < edited->GetNumSides(); side++ ) {
			if ( face == -1 || face == side ) {
				edited->GetSide( side )->SetMaterial( material );
			}
		}
		primitiveChanges.Append( Replace( world, brushes[i].entity, brushes[i].primitive, edited ) );
	}
	Edit( ( face == -1 ) ? "Set brush material" : "Set face material", idList<editorChange_t>(), primitiveChanges );
}

void idEditorEngineLocal::SetFaceTexture( const editorFaceTexture_t & texture ) {
	idList<brushRef_t> brushes;
	if ( !CanEdit() || !BrushesToEdit( brushes ) ) {
		return;
	}
	if ( texture.repeatS == 0.0f || texture.repeatT == 0.0f ) {
		common->Printf( "A texture cannot repeat every 0 units.\n" );
		return;
	}
	const int face = tool.SelectedFace();
	idList<editorPrimitiveChange_t> primitiveChanges;
	for ( int i = 0; i < brushes.Num(); i++ ) {
		idMapBrush * const edited = world.Brush( brushes[i].entity, brushes[i].primitive )->Clone();
		for ( int side = 0; side < edited->GetNumSides(); side++ ) {
			if ( face == -1 || face == side ) {
				editorBrush::SetTexture( *edited->GetSide( side ), texture.shiftS, texture.shiftT, texture.repeatS, texture.repeatT, texture.rotate );
			}
		}
		primitiveChanges.Append( Replace( world, brushes[i].entity, brushes[i].primitive, edited ) );
	}
	Edit( "Set texture", idList<editorChange_t>(), primitiveChanges );
}

void idEditorEngineLocal::HollowBrushes( float thickness ) {
	idList<brushRef_t> brushes;
	if ( !CanEdit() || !BrushesToEdit( brushes ) ) {
		return;
	}
	const float wall = Snap( thickness );
	// first every brush goes, then the walls are added behind what is left of each entity
	idList<editorPrimitiveChange_t> primitiveChanges;
	idList<editorPrimitiveChange_t> additions;
	idList<brushRef_t> made;
	for ( int i = 0; i < brushes.Num(); i++ ) {
		const int entity = brushes[i].entity;
		idList<idMapBrush *> walls;
		if ( !editorBrush::Hollow( *world.Brush( entity, brushes[i].primitive ), wall, walls ) ) {
			common->Printf( "Brush %d of entity %d is too small for walls of %g.\n", brushes[i].primitive, entity, wall );
			return;
		}
		primitiveChanges.Append( Remove( world, entity, brushes[i].primitive ) );
		int removed = 0;
		int added = 0;
		for ( int j = 0; j < brushes.Num(); j++ ) {
			removed += ( brushes[j].entity == entity ) ? 1 : 0;
		}
		for ( int j = 0; j < additions.Num(); j++ ) {
			added += ( additions[j].entity == entity ) ? 1 : 0;
		}
		for ( int j = 0; j < walls.Num(); j++ ) {
			const int primitive = world.NumPrimitives( entity ) - removed + added++;
			additions.Append( Insert( entity, primitive, walls[j] ) );
			made.Append( brushRef_t{ entity, primitive } );
		}
	}
	primitiveChanges.Append( additions );
	tool.ClearSelection();
	Edit( "Hollow", idList<editorChange_t>(), primitiveChanges );
	SelectBrushesOnly( made );
}

void idEditorEngineLocal::MakeBrushEntity( const char * classname ) {
	idList<brushRef_t> brushes;
	if ( !CanEdit() || classname == nullptr || !BrushesToEdit( brushes ) ) {
		return;
	}
	if ( gameEdit->FindEntityDefDict( classname, false ) == nullptr ) {
		common->Printf( "There is no entityDef '%s'.\n", classname );
		return;
	}
	idBounds bounds;
	bounds.Clear();
	for ( int i = 0; i < brushes.Num(); i++ ) {
		editorBrushWindings_t windings;
		if ( brushes[i].entity != 0 ) {
			common->Printf( "Only brushes of the worldspawn become an entity: editorMoveToWorld first.\n" );
			return;
		}
		if ( world.BrushWindings( 0, brushes[i].primitive, windings ) ) {
			bounds.AddBounds( editorBrush::Bounds( windings ) );
		}
	}
	if ( bounds.IsCleared() ) {
		return;
	}
	const idVec3 center = bounds.GetCenter();
	const idVec3 origin( Snap( center.x ), Snap( center.y ), Snap( center.z ) );

	// a brush entity finds its model of the .proc by its own name
	idList<editorChange_t> changes;
	editorChange_t & change = changes.Alloc();
	change.index = world.AddEntity();
	const idStr name = world.UniqueName( classname );
	change.after.Set( "classname", classname );
	change.after.Set( "name", name );
	change.after.Set( "model", name );
	change.after.SetVector( "origin", origin );

	// the last brush first, each put before the ones already moved: the order stays
	idList<editorPrimitiveChange_t> primitiveChanges;
	for ( int i = 0; i < brushes.Num(); i++ ) {
		idMapBrush * const moved = world.Brush( 0, brushes[i].primitive )->Clone();
		editorBrush::Rebase( *moved, vec3_origin, origin );
		primitiveChanges.Append( Remove( world, 0, brushes[i].primitive ) );
		primitiveChanges.Append( Insert( change.index, 0, moved ) );
	}
	tool.ClearSelection();
	Edit( va( "Make %s", classname ), changes, primitiveChanges );

	idList<int> created;
	created.Append( change.index );
	SelectOnly( created );
}

void idEditorEngineLocal::MoveBrushesToWorld() {
	if ( !CanEdit() ) {
		return;
	}
	idList<editorChange_t> changes;
	idList<editorPrimitiveChange_t> primitiveChanges;
	idList<brushRef_t> moved;
	int target = world.NumPrimitives( 0 );
	for ( int i = 0; i < tool.NumSelected(); i++ ) {
		const int entity = tool.Selected( i );
		if ( !IsEntity( entity ) || !world.HasPrimitives( entity ) ) {
			continue;
		}
		const editorMapEntity_t & owner = world.Entity( entity );
		if ( IsRotated( owner ) ) {
			common->Printf( "'%s' is rotated: its brushes are not moved.\n", owner.args.GetString( "name" ) );
			continue;
		}
		editorChange_t & change = changes.Alloc();
		change.index = entity;
		change.before = world.MapArgs( entity );
		// always the entity's first primitive: the ones before it are gone by then
		for ( int j = 0; j < world.NumPrimitives( entity ); j++ ) {
			editorPrimitiveChange_t removed = Remove( world, entity, j );
			removed.primitive = 0;
			primitiveChanges.Append( removed );
			idMapPrimitive * added;
			if ( const idMapBrush * const brush = world.Brush( entity, j ) ) {
				idMapBrush * const copy = brush->Clone();
				editorBrush::Rebase( *copy, owner.origin, vec3_origin );
				added = copy;
				moved.Append( brushRef_t{ 0, target } );
			} else {
				idMapPatch * const copy = static_cast<const idMapPatch *>( world.Primitive( entity, j ) )->Clone();
				for ( int v = 0; v < copy->GetNumVertices(); v++ ) {
					( *copy )[v].xyz += owner.origin;
				}
				added = copy;
			}
			primitiveChanges.Append( Insert( 0, target++, added ) );
		}
	}
	if ( changes.Num() == 0 ) {
		common->Printf( "No brush entity is selected.\n" );
		return;
	}
	tool.ClearSelection();
	Edit( "Move to world", changes, primitiveChanges );
	SelectBrushesOnly( moved );
}

void idEditorEngineLocal::PrintBrushInfo() const {
	if ( tool.NumSelectedBrushes() == 0 ) {
		common->Printf( "editorBrushInfo: no brush is selected\n" );
	}
	for ( int i = 0; i < tool.NumSelectedBrushes(); i++ ) {
		const brushRef_t & ref = tool.SelectedBrush( i );
		const idMapBrush * const brush = world.Brush( ref.entity, ref.primitive );
		if ( brush == nullptr ) {
			continue;
		}
		editorBrushWindings_t windings;
		const bool valid = editorBrush::Windings( *brush, windings );
		const idBounds bounds = editorBrush::Bounds( windings );
		common->Printf( "editorBrushInfo: entity %d primitive %d: %d sides, %d faces, %s, bounds (%g %g %g) (%g %g %g)\n", ref.entity, ref.primitive, brush->GetNumSides(),
			editorBrush::NumFaces( windings ), valid ? "valid" : "NO VOLUME", bounds[0].x, bounds[0].y, bounds[0].z, bounds[1].x, bounds[1].y, bounds[1].z );
		for ( int side = 0; side < brush->GetNumSides(); side++ ) {
			const idMapBrushSide & mapSide = *brush->GetSide( side );
			float shiftS, shiftT, repeatS, repeatT, rotate;
			editorBrush::GetTexture( mapSide, shiftS, shiftT, repeatS, repeatT, rotate );
			common->Printf( "  face %d%s: plane (%g %g %g %g) %s, shift %g %g, repeat %g %g, rotate %g\n", side, ( side == tool.SelectedFace() ) ? " (selected)" : "",
				mapSide.GetPlane()[0], mapSide.GetPlane()[1], mapSide.GetPlane()[2], mapSide.GetPlane()[3], mapSide.GetMaterial(), shiftS, shiftT, repeatS, repeatT, rotate );
		}
	}
}

void idEditorEngineLocal::PrintWorldInfo() const {
	if ( !world.IsOpen() ) {
		common->Printf( "editorWorldInfo: no map is open\n" );
		return;
	}
	int entities = 0;
	for ( int i = 0; i < world.NumEntities(); i++ ) {
		entities += world.Entity( i ).removed ? 0 : 1;
	}
	int brushes, patches, views, verts;
	world.CountPrimitives( brushes, patches, views, verts );
	PROCESS_MEMORY_COUNTERS_EX memory = {};
	memory.cb = sizeof( memory );
	GetProcessMemoryInfo( GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>( &memory ), sizeof( memory ) );
	common->Printf( "editorWorldInfo: %s, %s geometry%s, %d entities, %d brushes, %d patches, %d views, %d preview models of %d vertices, %d leak points, %d KB private\n",
		world.Name(), world.IsPreview() ? "preview" : "compiled", world.IsSuspended() ? " (suspended)" : "", entities, brushes, patches, views,
		idEditorWorld::NumPreviewModels(), verts, world.NumLeakPoints(), static_cast<int>( memory.PrivateUsage / 1024 ) );
}

CONSOLE_COMMAND_SHIP( editorCreateBrush, "adds an axial box to the worldspawn or the selected brush entity: editorCreateBrush <minx miny minz> <maxx maxy maxz> [material]", nullptr ) {
	if ( args.Argc() < 7 || args.Argc() > 8 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorCreateBrush <minx> <miny> <minz> <maxx> <maxy> <maxz> [material]\n" );
		return;
	}
	float corners[6];
	for ( int i = 0; i < 6; i++ ) {
		corners[i] = static_cast<float>( atof( args.Argv( 1 + i ) ) );
	}
	editorEngine->RunEdit( [&] { editorEngine->CreateBrush( corners, corners + 3, ( args.Argc() > 7 ) ? args.Argv( 7 ) : nullptr ); } );
}

CONSOLE_COMMAND_SHIP( editorSelectBrush, "selects a brush, and one of its faces: editorSelectBrush <entity name|world> <primitive> [face]", nullptr ) {
	if ( args.Argc() < 3 || args.Argc() > 4 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorSelectBrush <entity name|world> <primitive> [face]\n" );
		return;
	}
	const int entity = editorEngine->FindEntity( args.Argv( 1 ) );
	if ( entity != -1 ) {
		editorEngine->SelectBrush( entity, atoi( args.Argv( 2 ) ), ( args.Argc() > 3 ) ? atoi( args.Argv( 3 ) ) : -1, false );
	}
}

CONSOLE_COMMAND_SHIP( editorMoveBrush, "moves the selected brushes: editorMoveBrush <dx> <dy> <dz>", nullptr ) {
	if ( args.Argc() != 4 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorMoveBrush <dx> <dy> <dz>\n" );
		return;
	}
	const float delta[3] = { static_cast<float>( atof( args.Argv( 1 ) ) ), static_cast<float>( atof( args.Argv( 2 ) ) ), static_cast<float>( atof( args.Argv( 3 ) ) ) };
	editorEngine->RunEdit( [&] { editorEngine->MoveBrushes( delta ); } );
}

CONSOLE_COMMAND_SHIP( editorMoveFace, "pushes the selected face out of its brush, or pulls it in: editorMoveFace <distance>", nullptr ) {
	if ( args.Argc() != 2 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorMoveFace <distance>\n" );
		return;
	}
	editorEngine->RunEdit( [&] { editorEngine->MoveFace( static_cast<float>( atof( args.Argv( 1 ) ) ) ); } );
}

CONSOLE_COMMAND_SHIP( editorDeleteBrush, "removes the selected brushes", nullptr ) {
	if ( Editor_NeedEngine() ) {
		editorEngine->RunEdit( [] { editorEngine->DeleteSelection(); } );
	}
}

CONSOLE_COMMAND_SHIP( editorDuplicateBrush, "duplicates the selected brushes", nullptr ) {
	if ( Editor_NeedEngine() ) {
		editorEngine->RunEdit( [] { editorEngine->DuplicateSelection(); } );
	}
}

CONSOLE_COMMAND_SHIP( editorSetFaceMaterial, "gives the selected face a material, or every face of the selected brushes: editorSetFaceMaterial <material>", nullptr ) {
	if ( args.Argc() != 2 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorSetFaceMaterial <material>\n" );
		return;
	}
	editorEngine->RunEdit( [&] { editorEngine->SetFaceMaterial( args.Argv( 1 ) ); } );
}

CONSOLE_COMMAND_SHIP( editorSetFaceTexture, "lays the texture out on the selected face or brushes: editorSetFaceTexture <shiftS> <shiftT> <repeatS> <repeatT> <rotate>", nullptr ) {
	if ( args.Argc() != 6 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorSetFaceTexture <shiftS> <shiftT> <repeatS> <repeatT> <rotate>\n"
			"  shift in repeats, repeat in world units per repeat of the texture, rotate in degrees\n" );
		return;
	}
	editorFaceTexture_t texture;
	texture.shiftS = static_cast<float>( atof( args.Argv( 1 ) ) );
	texture.shiftT = static_cast<float>( atof( args.Argv( 2 ) ) );
	texture.repeatS = static_cast<float>( atof( args.Argv( 3 ) ) );
	texture.repeatT = static_cast<float>( atof( args.Argv( 4 ) ) );
	texture.rotate = static_cast<float>( atof( args.Argv( 5 ) ) );
	editorEngine->RunEdit( [&] { editorEngine->SetFaceTexture( texture ); } );
}

CONSOLE_COMMAND_SHIP( editorHollow, "turns every selected brush into one wall for each of its faces: editorHollow <thickness>", nullptr ) {
	if ( args.Argc() != 2 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorHollow <thickness>\n" );
		return;
	}
	editorEngine->RunEdit( [&] { editorEngine->HollowBrushes( static_cast<float>( atof( args.Argv( 1 ) ) ) ); } );
}

CONSOLE_COMMAND_SHIP( editorMakeEntity, "makes an entity of the selected brushes of the worldspawn: editorMakeEntity func_static", nullptr ) {
	if ( args.Argc() != 2 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorMakeEntity <entityDef>\n" );
		return;
	}
	editorEngine->RunEdit( [&] { editorEngine->MakeBrushEntity( args.Argv( 1 ) ); } );
}

CONSOLE_COMMAND_SHIP( editorMoveToWorld, "moves the brushes of the selected brush entities to the worldspawn and removes the entities", nullptr ) {
	if ( Editor_NeedEngine() ) {
		editorEngine->RunEdit( [] { editorEngine->MoveBrushesToWorld(); } );
	}
}

CONSOLE_COMMAND_SHIP( editorGrid, "the grid that moves and new brushes snap to, 0 = none: editorGrid <units>", nullptr ) {
	if ( args.Argc() != 2 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorGrid <units>\n" );
		return;
	}
	editorEngine->SetGridSize( static_cast<float>( atof( args.Argv( 1 ) ) ) );
}

CONSOLE_COMMAND_SHIP( editorBrushInfo, "prints the selected brushes: their planes, materials and textures", nullptr ) {
	if ( Editor_NeedEngine() ) {
		editorEngine->PrintBrushInfo();
	}
}

CONSOLE_COMMAND_SHIP( editorWorldInfo, "prints what the editor holds of the open map, and the memory of the process", nullptr ) {
	if ( Editor_NeedEngine() ) {
		editorEngine->PrintWorldInfo();
	}
}

CONSOLE_COMMAND_SHIP( editorPick, "prints what is under a pixel of the edit view: editorPick <x> <y>", nullptr ) {
	if ( args.Argc() != 3 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorPick <x> <y>\n" );
		return;
	}
	editorPickInfo_t pick;
	if ( editorEngine->Pick( atoi( args.Argv( 1 ) ), atoi( args.Argv( 2 ) ), pick ) ) {
		common->Printf( "editorPick: entity %d, primitive %d, face %d, distance %.1f, at %.1f %.1f %.1f\n", pick.entity, pick.primitive, pick.face, pick.distance, pick.point[0], pick.point[1], pick.point[2] );
	} else {
		common->Printf( "editorPick: nothing\n" );
	}
}

CONSOLE_COMMAND_SHIP( editorClick, "a click of the left button into the edit view: editorClick <x> <y> [ctrl]", nullptr ) {
	if ( args.Argc() < 3 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorClick <x> <y> [ctrl]\n" );
		return;
	}
	editorEngine->ViewMouseDown( atoi( args.Argv( 1 ) ), atoi( args.Argv( 2 ) ), idStr::Icmp( args.Argv( 3 ), "ctrl" ) == 0 );
	editorEngine->ViewMouseUp();
}

CONSOLE_COMMAND_SHIP( editorView, "puts the edit view's camera: editorView <x> <y> <z> [yaw] [pitch]", nullptr ) {
	if ( args.Argc() < 4 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorView <x> <y> <z> [yaw] [pitch]\n" );
		return;
	}
	const idVec3 origin( static_cast<float>( atof( args.Argv( 1 ) ) ), static_cast<float>( atof( args.Argv( 2 ) ) ), static_cast<float>( atof( args.Argv( 3 ) ) ) );
	editorEngine->SetView( origin, idAngles( static_cast<float>( atof( args.Argv( 5 ) ) ), static_cast<float>( atof( args.Argv( 4 ) ) ), 0.0f ) );
}

CONSOLE_COMMAND_SHIP( editorViewShot, "writes the edit view's next frame to a TGA file in fs_savepath: editorViewShot [file]", nullptr ) {
	if ( Editor_NeedEngine() ) {
		editorEngine->ViewShot( ( args.Argc() > 1 ) ? args.Argv( 1 ) : "editor_view.tga" );
	}
}
