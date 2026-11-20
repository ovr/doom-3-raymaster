// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include "d3xp/Game.h"
#include "renderer/Model.h"

#include "EditorWorld.h"

namespace {

// entities without a model are drawn as a box when the view is this near
constexpr float		MARKER_DISTANCE = 512.0f;
constexpr float		PLAYER_EYE_HEIGHT = 68.0f;
constexpr float		SKIN_MARGIN = 12.0f;				// how far a skin reaches beyond its joints
const idBounds		DEFAULT_MARKER_BOUNDS( idVec3( -8.0f, -8.0f, -8.0f ), idVec3( 8.0f, 8.0f, 8.0f ) );
constexpr float		CELL_SIZE = 512.0f;					// the preview's primitives share a model by the cell they are in
constexpr float		OUTLINE_DISTANCE = 1536.0f;			// portal, clip and trigger brushes are outlined this near
constexpr int		MAX_OUTLINE_LINES = 8000;			// of the renderer's 16384 debug lines
constexpr float		HEAD_LIGHT_RADIUS = 4096.0f;
constexpr float		PICK_SAME_SURFACE = 1.0f;			// a brush this near behind the traced surface is that surface
const idVec4		PORTAL_COLOR( 1.0f, 0.2f, 1.0f, 1.0f );
const idVec4		CLIP_COLOR( 1.0f, 0.9f, 0.2f, 1.0f );
const idVec4		TRIGGER_COLOR( 1.0f, 0.5f, 0.1f, 1.0f );
const idVec4		LEAK_COLOR( 1.0f, 0.0f, 0.0f, 1.0f );

// The brightness of the light at the camera while the geometry is a preview: a new map has no
// light of its own yet. 0 = only the map's lights, as in the compiled world.
idCVar editor_previewLight( "editor_previewLight", "0.6", CVAR_SYSTEM | CVAR_FLOAT, "the editor's light at the camera while brushes are shown uncompiled, 0 = off" );

// A shadow volume is made every frame for every light a model is in, out of the frame's index
// memory: fine for a room, an engine error for a retail map without areas to cull by.
idCVar editor_previewShadows( "editor_previewShadows", "0", CVAR_SYSTEM | CVAR_BOOL, "brushes shown uncompiled cast shadows; for small maps only" );

int					numPreviewModels = 0;

[[nodiscard]] idMapPrimitive * ClonePrimitive( const idMapPrimitive & primitive ) {
	if ( primitive.GetType() == idMapPrimitive::TYPE_BRUSH ) {
		return static_cast<const idMapBrush &>( primitive ).Clone();
	}
	return static_cast<const idMapPatch &>( primitive ).Clone();
}

[[nodiscard]] idBounds MarkerBounds( const idDict & args ) {
	idBounds bounds;
	if ( args.GetVector( "editor_mins", "", bounds[0] ) && args.GetVector( "editor_maxs", "", bounds[1] ) ) {
		return bounds;
	}
	if ( args.GetVector( "mins", "", bounds[0] ) && args.GetVector( "maxs", "", bounds[1] ) ) {
		return bounds;
	}
	return DEFAULT_MARKER_BOUNDS;
}

}	// namespace

// cleared for a brush that encloses no volume
static idBounds BoundsOfPrimitive( const idMapPrimitive & primitive ) {
	idBounds bounds;
	bounds.Clear();
	if ( primitive.GetType() == idMapPrimitive::TYPE_BRUSH ) {
		editorBrushWindings_t windings;
		if ( editorBrush::Windings( static_cast<const idMapBrush &>( primitive ), windings ) ) {
			bounds = editorBrush::Bounds( windings );
		}
	} else if ( primitive.GetType() == idMapPrimitive::TYPE_PATCH ) {
		bounds = editorBrush::PatchBounds( static_cast<const idMapPatch &>( primitive ) );
	}
	return bounds;
}

void idEditorWorld::MergedSpawnArgs( const idDict & mapArgs, idDict & out ) {
	out = mapArgs;
	const char * const classname = mapArgs.GetString( "classname" );
	if ( classname[0] == '\0' ) {
		return;
	}
	if ( const idDict * const defaults = gameEdit->FindEntityDefDict( classname, false ) ) {
		out.SetDefaults( defaults );
	}
}

bool idEditorWorld::Open( const char * name ) {
	Close();

	idStr cleanName = name;
	cleanName.StripFileExtension();
	cleanName.BackSlashesToSlashes();
	const idStr fileName = idStr( "maps/" ) + cleanName + ".map";

	const int start = Sys_Milliseconds();
	common->Printf( "--------- Editor: open %s ---------\n", cleanName.c_str() );

	// the map's own resource file holds its .map, its .bproc and the media only it uses
	fileSystem->BeginLevelLoad( cleanName, nullptr, 0 );
	renderSystem->BeginLevelLoad();
	declManager->BeginLevelLoad();

	const bool ok = mapFile.Parse( fileName, true, false, true );
	if ( !ok ) {
		common->Warning( "Editor: couldn't read %s", fileName.c_str() );
	} else {
		entities.SetGranularity( 1024 );
		entities.SetNum( mapFile.GetNumEntities() );
		for ( int i = 0; i < entities.Num(); i++ ) {
			MergedSpawnArgs( mapFile.GetEntity( i )->epairs, entities[i].args );
		}
		BuildWorld( fileName, ProcIsCurrent( fileName ) );
	}

	declManager->EndLevelLoad();
	renderSystem->EndLevelLoad();
	fileSystem->EndLevelLoad();

	if ( !ok ) {
		Close();
		fileSystem->UnloadMapResources( cleanName );
		return false;
	}
	mapName = cleanName;
	common->Printf( "%d entities, %s geometry, %d msec to open %s\n", entities.Num(), previewGeometry ? "preview" : "compiled", Sys_Milliseconds() - start, cleanName.c_str() );
	return true;
}

void idEditorWorld::New( const char * name ) {
	Close();

	idStr cleanName = name;
	cleanName.StripFileExtension();
	cleanName.BackSlashesToSlashes();
	common->Printf( "--------- Editor: new %s ---------\n", cleanName.c_str() );

	idMapEntity * const worldspawn = new ( TAG_IDLIB ) idMapEntity;
	worldspawn->epairs.Set( "classname", "worldspawn" );
	mapFile.AddEntity( worldspawn );
	entities.SetGranularity( 1024 );
	entities.SetNum( 1 );
	MergedSpawnArgs( worldspawn->epairs, entities[0].args );

	mapName = cleanName;
	unsaved = true;
	BuildWorld( idStr( "maps/" ) + cleanName + ".map", false );
}

// The .cm of a compile carries the geometry CRC of the .map it was made from. The CRC does not
// see a texture matrix: a map saved with only such a change still counts as compiled.
bool idEditorWorld::ProcIsCurrent( const char * fileName ) const {
	idStr procName = fileName;
	procName.SetFileExtension( "proc" );
	const ID_TIME_T procTime = fileSystem->GetTimestamp( procName );
	if ( procTime == FILE_NOT_FOUND_TIMESTAMP || procTime == 0 ) {
		// the game's own: only a binary one in the resource files, if any
		return true;
	}
	idStr cmName = fileName;
	cmName.SetFileExtension( "cm" );
	idLexer src( cmName, LEXFL_NOSTRINGESCAPECHARS | LEXFL_NOERRORS | LEXFL_NOWARNINGS );
	idToken token;
	if ( !src.IsLoaded() || !src.ReadToken( &token ) || !src.ReadToken( &token ) || !src.ReadToken( &token ) || token.type != TT_NUMBER ) {
		return true;
	}
	const unsigned int crc = token.GetUnsignedLongValue();
	if ( crc == mapFile.GetGeometryCRC() ) {
		return true;
	}
	// the compiler read the map with its worldspawn directives applied
	idMapFile asCompiled;
	if ( asCompiled.Parse( fileName ) && asCompiled.GetGeometryCRC() == crc ) {
		return true;
	}
	common->Printf( "Editor: %s is not of this %s: the geometry is shown uncompiled\n", procName.c_str(), fileName );
	return false;
}

void idEditorWorld::BuildWorld( const char * fileName, bool compiled ) {
	world = renderSystem->AllocRenderWorld();
	previewGeometry = !( compiled && world->InitFromMap( fileName ) );
	if ( previewGeometry ) {
		// an empty world of one area
		world->InitFromMap( nullptr );
		StartInteractions();
	}
	for ( int i = 0; i < entities.Num(); i++ ) {
		ShowEntity( i );
	}
	if ( !previewGeometry ) {
		// as the game does after spawning; the frontend also needs the interaction table this makes
		world->GenerateAllInteractions();
	}
}

// The frontend needs the interaction table. Made before anything is in the world, it holds no
// static interaction: those draw from static vertex buffers, which only a level load fills, and
// the models of the preview are made at any time.
void idEditorWorld::StartInteractions() {
	world->GenerateAllInteractions();
}

void idEditorWorld::FreeWorld() {
	for ( int i = 0; i < entities.Num(); i++ ) {
		FreeEntity( entities[i] );
	}
	if ( world != nullptr ) {
		if ( headLightHandle != -1 ) {
			world->FreeLightDef( headLightHandle );
		}
		renderSystem->FreeRenderWorld( world );
		world = nullptr;
	}
	headLightHandle = -1;
}

void idEditorWorld::Suspend() {
	FreeWorld();
	// the game mounts the map's resource file itself
	fileSystem->UnloadMapResources( mapName );
}

bool idEditorWorld::Resume() {
	assert( IsSuspended() );
	const int start = Sys_Milliseconds();
	// the copy of the resource file the game mounted; BeginLevelLoad mounts the editor's
	fileSystem->UnloadMapResources( mapName );
	fileSystem->BeginLevelLoad( mapName, nullptr, 0 );
	renderSystem->BeginLevelLoad();
	declManager->BeginLevelLoad();
	BuildWorld( idStr( "maps/" ) + mapName + ".map", !previewGeometry );
	declManager->EndLevelLoad();
	renderSystem->EndLevelLoad();
	fileSystem->EndLevelLoad();
	common->Printf( "%d msec to show %s again, %s geometry\n", Sys_Milliseconds() - start, mapName.c_str(), previewGeometry ? "preview" : "compiled" );
	return true;
}

bool idEditorWorld::ShowCompiled() {
	if ( !IsOpen() ) {
		return false;
	}
	Suspend();
	previewGeometry = false;
	return Resume() && !previewGeometry;
}

void idEditorWorld::ShowPreview() {
	if ( previewGeometry || world == nullptr ) {
		return;
	}
	const int start = Sys_Milliseconds();
	for ( int i = 0; i < entities.Num(); i++ ) {
		FreeEntity( entities[i] );
	}
	if ( headLightHandle != -1 ) {
		world->FreeLightDef( headLightHandle );
		headLightHandle = -1;
	}
	// frees the areas and the models of the .proc
	world->InitFromMap( nullptr );
	StartInteractions();
	previewGeometry = true;
	for ( int i = 0; i < entities.Num(); i++ ) {
		ShowEntity( i );
	}
	common->Printf( "Editor: the geometry is shown uncompiled from now on (%d msec)\n", Sys_Milliseconds() - start );
}

void idEditorWorld::Close() {
	FreeWorld();
	primitiveBounds.DeleteContents( true );
	entities.Clear();
	mapFile.RemoveAllEntities();
	leakPoints.Clear();
	previewGeometry = false;
	unsaved = false;
	if ( !mapName.IsEmpty() ) {
		fileSystem->UnloadMapResources( mapName );
		mapName.Clear();
	}
}

bool idEditorWorld::Save() {
	if ( !IsOpen() || !mapFile.Write( idStr( "maps/" ) + mapName, ".map", "fs_savepath" ) ) {
		return false;
	}
	unsaved = false;
	return true;
}

void idEditorWorld::SetMapArgs( int index, const idDict & mapArgs ) {
	mapFile.GetEntity( index )->epairs = mapArgs;
	MergedSpawnArgs( mapArgs, entities[index].args );
	ShowEntity( index );
}

const idMapPrimitive * idEditorWorld::Primitive( int entity, int primitive ) const {
	if ( entity < 0 || entity >= entities.Num() || primitive < 0 || primitive >= NumPrimitives( entity ) ) {
		return nullptr;
	}
	return mapFile.GetEntity( entity )->GetPrimitive( primitive );
}

const idMapBrush * idEditorWorld::Brush( int entity, int primitive ) const {
	const idMapPrimitive * const found = Primitive( entity, primitive );
	return ( found != nullptr && found->GetType() == idMapPrimitive::TYPE_BRUSH ) ? static_cast<const idMapBrush *>( found ) : nullptr;
}

void idEditorWorld::ApplyPrimitive( const editorPrimitiveChange_t & change, bool forward ) {
	const idMapPrimitive * const before = ( forward ? change.before : change.after ).get();
	const idMapPrimitive * const after = ( forward ? change.after : change.before ).get();
	ShowPreview();

	idMapEntity * const mapEntity = mapFile.GetEntity( change.entity );
	editorMapEntity_t & entity = entities[change.entity];
	// the worldspawn can have thousands of primitives: only the changed one is made again
	const bool oneView = ( change.entity == 0 && world != nullptr && entity.views != nullptr && entity.views->Num() == mapEntity->GetNumPrimitives() );

	idList<idBounds> * const cached = ( change.entity < primitiveBounds.Num() ) ? primitiveBounds[change.entity] : nullptr;
	if ( cached != nullptr ) {
		if ( before != nullptr ) {
			cached->RemoveIndex( change.primitive );
		}
		if ( after != nullptr ) {
			cached->Insert( BoundsOfPrimitive( *after ), change.primitive );
		}
	}

	if ( before != nullptr ) {
		delete mapEntity->RemovePrimitive( change.primitive );
		if ( oneView ) {
			const int cell = ( *entity.views )[change.primitive].cell;
			if ( cell != -1 ) {
				( *entity.cells )[cell].dirty = true;
			}
			entity.views->RemoveIndex( change.primitive );
		}
	}
	if ( after != nullptr ) {
		mapEntity->InsertPrimitive( change.primitive, ClonePrimitive( *after ) );
		if ( oneView ) {
			entity.views->Insert( editorPrimitiveView_t(), change.primitive );
			MakeView( change.entity, change.primitive, ( *entity.views )[change.primitive] );
		}
	}
	if ( oneView ) {
		BuildCells( change.entity );
		UpdateViewBounds( entity );
	} else if ( world != nullptr && !entity.removed ) {
		ShowEntity( change.entity );
	}
}

const idList<idBounds> & idEditorWorld::PrimitiveBounds( int entity ) const {
	while ( primitiveBounds.Num() <= entity ) {
		primitiveBounds.Append( nullptr );
	}
	if ( primitiveBounds[entity] == nullptr ) {
		auto * const list = new ( TAG_IDLIB ) idList<idBounds>;
		const idMapEntity * const mapEntity = mapFile.GetEntity( entity );
		list->SetNum( mapEntity->GetNumPrimitives() );
		for ( int i = 0; i < list->Num(); i++ ) {
			( *list )[i] = BoundsOfPrimitive( *mapEntity->GetPrimitive( i ) );
		}
		primitiveBounds[entity] = list;
	}
	return *primitiveBounds[entity];
}

bool idEditorWorld::BrushWindings( int entity, int primitive, editorBrushWindings_t & windings ) const {
	const idMapBrush * const brush = Brush( entity, primitive );
	if ( brush == nullptr || !editorBrush::Windings( *brush, windings ) ) {
		return false;
	}
	const editorMapEntity_t & owner = entities[entity];
	for ( int i = 0; i < windings.Num(); i++ ) {
		for ( int j = 0; j < windings[i].GetNumPoints(); j++ ) {
			windings[i][j].ToVec3() = owner.origin + windings[i][j].ToVec3() * owner.axis;
		}
	}
	return true;
}

void idEditorWorld::CountPrimitives( int & brushes, int & patches, int & views, int & verts ) const {
	brushes = patches = views = verts = 0;
	for ( int i = 0; i < entities.Num(); i++ ) {
		const idMapEntity * const mapEntity = mapFile.GetEntity( i );
		if ( entities[i].removed ) {
			continue;
		}
		for ( int j = 0; j < mapEntity->GetNumPrimitives(); j++ ) {
			if ( mapEntity->GetPrimitive( j )->GetType() == idMapPrimitive::TYPE_BRUSH ) {
				brushes++;
			} else {
				patches++;
			}
		}
		views += ( entities[i].views != nullptr ) ? entities[i].views->Num() : 0;
		for ( int j = 0; entities[i].cells != nullptr && j < entities[i].cells->Num(); j++ ) {
			const idRenderModel * const model = ( *entities[i].cells )[j].model;
			for ( int s = 0; model != nullptr && s < model->NumSurfaces(); s++ ) {
				verts += model->Surface( s )->geometry->numVerts;
			}
		}
	}
}

int idEditorWorld::NumPreviewModels() {
	return numPreviewModels;
}

bool idEditorWorld::LoadLeak( const char * fileName ) {
	leakPoints.Clear();
	idLexer src( fileName, LEXFL_NOERRORS | LEXFL_NOWARNINGS );
	if ( !src.IsLoaded() ) {
		return false;
	}
	while ( !src.EndOfFile() ) {
		idVec3 point;
		bool error = false;
		for ( int axis = 0; axis < 3 && !error; axis++ ) {
			point[axis] = src.ParseFloat( &error );
		}
		if ( error ) {
			break;
		}
		leakPoints.Append( point );
	}
	return leakPoints.Num() > 0;
}

void idEditorWorld::MakeView( int index, int primitive, editorPrimitiveView_t & view ) {
	editorMapEntity_t & entity = entities[index];
	const idMapPrimitive * const mapPrimitive = mapFile.GetEntity( index )->GetPrimitive( primitive );
	view.bounds.Clear();
	view.kind = editorBrushKind_t::HIDDEN;
	view.outline.Clear();
	view.cell = -1;

	if ( mapPrimitive->GetType() == idMapPrimitive::TYPE_BRUSH ) {
		const idMapBrush & brush = *static_cast<const idMapBrush *>( mapPrimitive );
		editorBrushWindings_t windings;
		if ( !editorBrush::Windings( brush, windings ) ) {
			return;
		}
		view.bounds = editorBrush::Bounds( windings );
		view.kind = editorBrush::Kind( brush );
		if ( view.kind != editorBrushKind_t::DRAWN && view.kind != editorBrushKind_t::HIDDEN ) {
			for ( int i = 0; i < windings.Num(); i++ ) {
				const int numPoints = windings[i].GetNumPoints();
				for ( int j = 0; j < numPoints; j++ ) {
					view.outline.Append( windings[i][j].ToVec3() );
					view.outline.Append( windings[i][( j + 1 ) % numPoints].ToVec3() );
				}
			}
		}
	} else if ( mapPrimitive->GetType() == idMapPrimitive::TYPE_PATCH ) {
		const idMapPatch & patch = *static_cast<const idMapPatch *>( mapPrimitive );
		if ( editorBrush::PatchIsDrawn( patch ) ) {
			view.kind = editorBrushKind_t::DRAWN;
			view.bounds = editorBrush::PatchBounds( patch );
		}
	}
	if ( view.kind != editorBrushKind_t::DRAWN || view.bounds.IsCleared() ) {
		return;
	}

	// the cell its middle is in; a cell is never taken out of the list, so the index holds
	const idVec3 center = view.bounds.GetCenter();
	const int key[3] = { static_cast<int>( idMath::Floor( center.x / CELL_SIZE ) ), static_cast<int>( idMath::Floor( center.y / CELL_SIZE ) ), static_cast<int>( idMath::Floor( center.z / CELL_SIZE ) ) };
	idList<editorGeometryCell_t> & cells = *entity.cells;
	for ( int i = 0; i < cells.Num() && view.cell == -1; i++ ) {
		if ( cells[i].key[0] == key[0] && cells[i].key[1] == key[1] && cells[i].key[2] == key[2] ) {
			view.cell = i;
		}
	}
	if ( view.cell == -1 ) {
		editorGeometryCell_t cell;
		cell.key[0] = key[0];
		cell.key[1] = key[1];
		cell.key[2] = key[2];
		view.cell = cells.Append( cell );
	}
	cells[view.cell].dirty = true;
}

void idEditorWorld::FreeCell( editorGeometryCell_t & cell ) {
	if ( cell.handle != -1 && world != nullptr ) {
		world->FreeEntityDef( cell.handle );
	}
	cell.handle = -1;
	if ( cell.model != nullptr ) {
		renderModelManager->FreeModel( cell.model );
		cell.model = nullptr;
		numPreviewModels--;
	}
}

// Makes the model of every cell a view was added to or taken from.
void idEditorWorld::BuildCells( int index ) {
	editorMapEntity_t & entity = entities[index];
	idList<editorGeometryCell_t> & cells = *entity.cells;
	const idMapEntity * const mapEntity = mapFile.GetEntity( index );

	idList<idEditorModelBuilder *> builders;
	builders.SetNum( cells.Num() );
	for ( int i = 0; i < cells.Num(); i++ ) {
		builders[i] = cells[i].dirty ? new ( TAG_IDLIB ) idEditorModelBuilder : nullptr;
	}
	for ( int i = 0; i < entity.views->Num(); i++ ) {
		const int cell = ( *entity.views )[i].cell;
		if ( cell == -1 || builders[cell] == nullptr ) {
			continue;
		}
		const idMapPrimitive * const mapPrimitive = mapEntity->GetPrimitive( i );
		if ( mapPrimitive->GetType() == idMapPrimitive::TYPE_BRUSH ) {
			const idMapBrush & brush = *static_cast<const idMapBrush *>( mapPrimitive );
			editorBrushWindings_t windings;
			if ( editorBrush::Windings( brush, windings ) ) {
				builders[cell]->AddBrush( brush, windings, entity.origin );
			}
		} else {
			builders[cell]->AddPatch( *static_cast<const idMapPatch *>( mapPrimitive ) );
		}
	}
	for ( int i = 0; i < cells.Num(); i++ ) {
		if ( builders[i] == nullptr ) {
			continue;
		}
		editorGeometryCell_t & cell = cells[i];
		FreeCell( cell );
		cell.dirty = false;
		cell.model = builders[i]->Finish();
		delete builders[i];
		if ( cell.model == nullptr ) {
			continue;
		}
		numPreviewModels++;
		// the entity's origin, axis and shader parms; ShowPrimitives() made them
		renderEntity_t parms = entity.renderEntity;
		parms.hModel = cell.model;
		parms.bounds = cell.model->Bounds();
		parms.entityNum = 0;
		parms.noShadow = !editor_previewShadows.GetBool();
		cell.handle = world->AddEntityDef( &parms );
	}
}

void idEditorWorld::FreeViews( editorMapEntity_t & entity ) {
	if ( entity.views == nullptr ) {
		return;
	}
	for ( int i = 0; i < entity.cells->Num(); i++ ) {
		FreeCell( ( *entity.cells )[i] );
	}
	delete entity.cells;
	entity.cells = nullptr;
	delete entity.views;
	entity.views = nullptr;
}

void idEditorWorld::UpdateViewBounds( editorMapEntity_t & entity ) {
	entity.viewBounds.Clear();
	for ( int i = 0; entity.views != nullptr && i < entity.views->Num(); i++ ) {
		if ( !( *entity.views )[i].bounds.IsCleared() ) {
			entity.viewBounds.AddBounds( ( *entity.views )[i].bounds );
		}
	}
}

void idEditorWorld::ShowPrimitives( int index ) {
	editorMapEntity_t & entity = entities[index];
	const int numPrimitives = mapFile.GetEntity( index )->GetNumPrimitives();
	// a key of the worldspawn changes nothing of its geometry
	if ( index == 0 && entity.views != nullptr && entity.views->Num() == numPrimitives ) {
		return;
	}
	FreeViews( entity );

	// what every view of the entity shares; not = {}, see renderEntity_t
	memset( &entity.renderEntity, 0, sizeof( entity.renderEntity ) );
	entity.renderEntity.axis = mat3_identity;
	if ( index != 0 ) {
		// the "model" of a brush entity is its name: the model of the .proc, which is not loaded
		idDict args = entity.args;
		args.Delete( "model" );
		gameEdit->ParseSpawnArgsToRenderEntity( &args, &entity.renderEntity );
	} else {
		entity.renderEntity.shaderParms[SHADERPARM_RED] = entity.renderEntity.shaderParms[SHADERPARM_GREEN] = 1.0f;
		entity.renderEntity.shaderParms[SHADERPARM_BLUE] = entity.renderEntity.shaderParms[SHADERPARM_ALPHA] = 1.0f;
	}
	entity.renderEntity.hModel = nullptr;
	entity.origin = entity.renderEntity.origin;
	entity.axis = entity.renderEntity.axis;

	entity.views = new ( TAG_IDLIB ) idList<editorPrimitiveView_t>;
	entity.cells = new ( TAG_IDLIB ) idList<editorGeometryCell_t>;
	entity.views->SetNum( numPrimitives );
	for ( int i = 0; i < numPrimitives; i++ ) {
		MakeView( index, i, ( *entity.views )[i] );
	}
	BuildCells( index );
	UpdateViewBounds( entity );
}

int idEditorWorld::AddEntity() {
	const int index = mapFile.AddEntity( new ( TAG_IDLIB ) idMapEntity );
	entities.Alloc().removed = true;
	assert( entities.Num() == mapFile.GetNumEntities() );
	return index;
}

int idEditorWorld::FindEntity( const char * name ) const {
	for ( int i = 1; i < entities.Num(); i++ ) {
		if ( !entities[i].removed && idStr::Icmp( entities[i].args.GetString( "name" ), name ) == 0 ) {
			return i;
		}
	}
	return -1;
}

idStr idEditorWorld::UniqueName( const char * base ) const {
	for ( int n = 1; ; n++ ) {
		const idStr name = va( "%s_%d", base, n );
		if ( FindEntity( name ) == -1 ) {
			return name;
		}
	}
}

void idEditorWorld::ShowEntity( int index ) {
	editorMapEntity_t & entity = entities[index];
	entity.removed = ( index != 0 && entity.args.GetNumKeyVals() == 0 );
	if ( entity.removed ) {
		FreeEntity( entity );
		entity.isLight = false;
		return;
	}
	entity.origin = entity.args.GetVector( "origin", "0 0 0" );
	entity.axis = mat3_identity;
	entity.bounds = MarkerBounds( entity.args );

	const bool asPrimitives = previewGeometry && HasPrimitives( index );
	if ( asPrimitives ) {
		ShowPrimitives( index );
	} else {
		FreeViews( entity );
	}
	// the worldspawn's geometry is otherwise the world's areas, which InitFromMap already made
	if ( index == 0 ) {
		return;
	}

	entity.isLight = ( idStr::Icmp( entity.args.GetString( "spawnclass" ), "idLight" ) == 0 );
	if ( entity.isLight ) {
		gameEdit->ParseSpawnArgsToRenderLight( &entity.args, &entity.renderLight );
		if ( entity.lightHandle == -1 ) {
			entity.lightHandle = world->AddLightDef( &entity.renderLight );
		} else {
			world->UpdateLightDef( entity.lightHandle, &entity.renderLight );
		}
	} else if ( entity.lightHandle != -1 ) {
		world->FreeLightDef( entity.lightHandle );
		entity.lightHandle = -1;
	}

	bool hasModel = false;
	// in the preview the model of a brush entity, the one named like it, is not loaded
	const bool procModel = previewGeometry && idStr::Icmp( entity.args.GetString( "model" ), entity.args.GetString( "name" ) ) == 0;
	if ( !asPrimitives && !procModel && entity.args.GetString( "model" )[0] != '\0' ) {
		gameEdit->ParseSpawnArgsToRenderEntity( &entity.args, &entity.renderEntity );
		hasModel = ( entity.renderEntity.hModel != nullptr && !entity.renderEntity.hModel->IsDefaultModel() );
		entity.axis = entity.renderEntity.axis;
	}
	if ( !hasModel ) {
		if ( entity.modelHandle != -1 ) {
			world->FreeEntityDef( entity.modelHandle );
			entity.modelHandle = -1;
		}
		return;
	}

	// Pick() finds the map entity of a hit model by this
	entity.renderEntity.entityNum = index;
	if ( entity.renderEntity.hModel->NumJoints() > 0 ) {
		PoseSkinnedModel( entity.args, entity.renderEntity, entity.joints );
	}
	if ( entity.modelHandle == -1 ) {
		entity.modelHandle = world->AddEntityDef( &entity.renderEntity );
	} else {
		world->UpdateEntityDef( entity.modelHandle, &entity.renderEntity );
	}
}

void idEditorWorld::PoseSkinnedModel( const idDict & args, renderEntity_t & renderEntity, idJointMat * & joints ) {
	const idRenderModel * const model = renderEntity.hModel;
	const int numJoints = model->NumJoints();
	if ( joints != nullptr ) {
		Mem_Free16( joints );
	}
	joints = static_cast<idJointMat *>( Mem_Alloc16( SIMD_ROUND_JOINTS( numJoints ) * sizeof( idJointMat ), TAG_JOINTMAT ) );

	const char * const classname = args.GetString( "classname" );
	const idMD5Anim * const anim = ( classname[0] != '\0' ) ? gameEdit->ANIM_GetAnimFromEntityDef( classname, args.GetString( "anim", "idle" ) ) : nullptr;
	if ( anim != nullptr && gameEdit->ANIM_GetNumFrames( anim ) > 0 ) {
		gameEdit->ANIM_CreateAnimFrame( model, anim, numJoints, joints, 1, vec3_origin, false );
	} else {
		SIMDProcessor->ConvertJointQuatsToJointMats( joints, model->GetDefaultPose(), numJoints );
		const idMD5Joint * const md5Joints = model->GetJoints();
		for ( int i = 1; i < numJoints; i++ ) {
			if ( md5Joints[i].parent != nullptr ) {
				joints[i] *= joints[static_cast<int>( md5Joints[i].parent - md5Joints )];
			}
		}
	}
	SIMD_INIT_LAST_JOINT( joints, numJoints );

	renderEntity.joints = joints;
	renderEntity.numJoints = numJoints;
	// a skinned model has the bounds its entity gives it: around the joints, with room for the skin
	renderEntity.bounds.Clear();
	for ( int i = 0; i < numJoints; i++ ) {
		renderEntity.bounds.AddPoint( joints[i].ToVec3() );
	}
	renderEntity.bounds.ExpandSelf( SKIN_MARGIN );
}

void idEditorWorld::FreeEntity( editorMapEntity_t & entity ) {
	FreeViews( entity );
	if ( world != nullptr ) {
		if ( entity.modelHandle != -1 ) {
			world->FreeEntityDef( entity.modelHandle );
		}
		if ( entity.lightHandle != -1 ) {
			world->FreeLightDef( entity.lightHandle );
		}
	}
	entity.modelHandle = -1;
	entity.lightHandle = -1;
	if ( entity.joints != nullptr ) {
		Mem_Free16( entity.joints );
		entity.joints = nullptr;
	}
}

idBounds idEditorWorld::WorldBounds( int index ) const {
	const editorMapEntity_t & entity = entities[index];
	idBounds bounds;
	if ( entity.modelHandle != -1 ) {
		bounds.FromTransformedBounds( entity.renderEntity.bounds, entity.renderEntity.origin, entity.renderEntity.axis );
	} else if ( entity.views != nullptr && !entity.viewBounds.IsCleared() ) {
		bounds.FromTransformedBounds( entity.viewBounds, entity.origin, entity.axis );
	} else {
		bounds = entity.bounds.Translate( entity.origin );
	}
	return bounds;
}

editorPick_t idEditorWorld::Pick( const idVec3 & start, const idVec3 & dir, float maxDistance ) const {
	editorPick_t pick;
	if ( world == nullptr ) {
		return pick;
	}
	pick.distance = maxDistance;

	// models, and the world's geometry (the areas, the preview models) as entity 0
	modelTrace_t trace = {};
	if ( world->Trace( trace, start, start + dir * maxDistance, 0.0f, false ) && trace.entity != nullptr ) {
		pick.distance = trace.fraction * maxDistance;
		const int index = trace.entity->entityNum;
		pick.entity = ( index > 0 && index < entities.Num() ) ? index : -1;
	}

	// the boxes of what has neither a model nor primitives that are shown
	for ( int i = 1; i < entities.Num(); i++ ) {
		const editorMapEntity_t & entity = entities[i];
		if ( entity.modelHandle != -1 || entity.removed || entity.views != nullptr ) {
			continue;
		}
		float scale = 0.0f;
		if ( entity.bounds.Translate( entity.origin ).RayIntersection( start, dir, scale ) && scale >= 0.0f && scale < pick.distance ) {
			pick.distance = scale;
			pick.entity = i;
		}
	}

	// The primitives: which of them is the surface the trace hit, or an outlined brush before it.
	editorPick_t primitive;
	primitive.distance = pick.distance + ( ( pick.entity <= 0 || HasPrimitives( pick.entity ) ) ? PICK_SAME_SURFACE : 0.0f );
	for ( int i = 0; i < entities.Num(); i++ ) {
		const editorMapEntity_t & entity = entities[i];
		const int numPrimitives = NumPrimitives( i );
		// compiled: a brush entity is seen when it has its model of the .proc
		if ( entity.removed || numPrimitives == 0 || ( !previewGeometry && i != 0 && entity.modelHandle == -1 ) ) {
			continue;
		}
		const idMat3 inverse = entity.axis.Transpose();
		const idVec3 localStart = ( start - entity.origin ) * inverse;
		const idVec3 localDir = dir * inverse;
		for ( int j = 0; j < numPrimitives; j++ ) {
			const editorPrimitiveView_t * const view = ( entity.views != nullptr && j < entity.views->Num() ) ? &( *entity.views )[j] : nullptr;
			if ( view != nullptr && ( view->kind == editorBrushKind_t::HIDDEN || view->bounds.IsCleared() ) ) {
				continue;
			}
			float scale = 0.0f;
			if ( view != nullptr && ( !view->bounds.RayIntersection( localStart, localDir, scale ) || scale > primitive.distance ) && !view->bounds.ContainsPoint( localStart ) ) {
				continue;
			}
			const idMapPrimitive * const mapPrimitive = mapFile.GetEntity( i )->GetPrimitive( j );
			float distance = 0.0f;
			int side = -1;
			if ( mapPrimitive->GetType() == idMapPrimitive::TYPE_BRUSH ) {
				const idMapBrush & brush = *static_cast<const idMapBrush *>( mapPrimitive );
				if ( !editorBrush::Ray( brush, localStart, localDir, distance, side ) || distance >= primitive.distance ) {
					continue;
				}
				const bool outlined = ( view != nullptr && view->kind != editorBrushKind_t::DRAWN );
				if ( !outlined && !declManager->FindMaterial( brush.GetSide( side )->GetMaterial() )->IsDrawn() ) {
					continue;
				}
			} else if ( view == nullptr || !editorBrush::RayPatch( *static_cast<const idMapPatch *>( mapPrimitive ), localStart, localDir, distance ) || distance >= primitive.distance ) {
				continue;
			}
			primitive.entity = i;
			primitive.primitive = j;
			primitive.face = side;
			primitive.distance = distance;
		}
	}
	return ( primitive.entity != -1 ) ? primitive : pick;
}

void idEditorWorld::GetStartView( idVec3 & origin, idAngles & angles ) const {
	angles.Zero();
	origin.Zero();

	for ( int i = 0; i < entities.Num(); i++ ) {
		const editorMapEntity_t & entity = entities[i];
		if ( idStr::Icmp( entity.args.GetString( "classname" ), "info_player_start" ) == 0 ) {
			origin = entity.origin + idVec3( 0.0f, 0.0f, PLAYER_EYE_HEIGHT );
			angles.yaw = entity.args.GetFloat( "angle" );
			return;
		}
	}

	idBounds all;
	all.Clear();
	for ( int i = 1; i < entities.Num(); i++ ) {
		if ( !entities[i].removed ) {
			all.AddPoint( entities[i].origin );
		}
	}
	if ( !all.IsCleared() ) {
		origin = all.GetCenter();
	}
}

void idEditorWorld::DrawMarkers( const idVec3 & viewOrigin ) {
	const float maxDistanceSqr = Square( MARKER_DISTANCE );
	for ( int i = 1; i < entities.Num(); i++ ) {
		const editorMapEntity_t & entity = entities[i];
		if ( entity.modelHandle != -1 || entity.removed || entity.views != nullptr || ( entity.origin - viewOrigin ).LengthSqr() > maxDistanceSqr ) {
			continue;
		}
		idVec3 color = entity.isLight ? entity.args.GetVector( "_color", "1 1 1" ) : entity.args.GetVector( "editor_color", "0.3 0.6 1" );
		world->DebugBounds( idVec4( color.x, color.y, color.z, 1.0f ), entity.bounds, entity.origin );
	}

	// the brushes the game does not draw but the mapper works with
	int lines = 0;
	for ( int i = 0; i < entities.Num() && lines < MAX_OUTLINE_LINES; i++ ) {
		const editorMapEntity_t & entity = entities[i];
		if ( entity.removed || entity.views == nullptr ) {
			continue;
		}
		for ( int j = 0; j < entity.views->Num() && lines < MAX_OUTLINE_LINES; j++ ) {
			const editorPrimitiveView_t & view = ( *entity.views )[j];
			if ( view.outline.Num() == 0 || ( entity.origin + view.bounds.GetCenter() * entity.axis - viewOrigin ).LengthSqr() > Square( OUTLINE_DISTANCE ) ) {
				continue;
			}
			const idVec4 & color = ( view.kind == editorBrushKind_t::PORTAL ) ? PORTAL_COLOR : ( ( view.kind == editorBrushKind_t::CLIP ) ? CLIP_COLOR : TRIGGER_COLOR );
			for ( int k = 0; k + 1 < view.outline.Num(); k += 2 ) {
				world->DebugLine( color, entity.origin + view.outline[k] * entity.axis, entity.origin + view.outline[k + 1] * entity.axis, 0, true );
			}
			lines += view.outline.Num() / 2;
		}
	}

	if ( showLeak ) {
		for ( int i = 0; i + 1 < leakPoints.Num(); i++ ) {
			world->DebugLine( LEAK_COLOR, leakPoints[i], leakPoints[i + 1] );
		}
	}
}

void idEditorWorld::UpdateHeadLight( const idVec3 & viewOrigin ) {
	const float brightness = previewGeometry ? editor_previewLight.GetFloat() : 0.0f;
	if ( brightness <= 0.0f ) {
		if ( headLightHandle != -1 ) {
			world->FreeLightDef( headLightHandle );
			headLightHandle = -1;
		}
		return;
	}
	if ( headLightHandle != -1 && headLight.origin == viewOrigin && headLight.shaderParms[SHADERPARM_RED] == brightness ) {
		return;
	}
	memset( &headLight, 0, sizeof( headLight ) );
	headLight.axis = mat3_identity;
	headLight.origin = viewOrigin;
	headLight.pointLight = true;
	headLight.lightRadius.Set( HEAD_LIGHT_RADIUS, HEAD_LIGHT_RADIUS, HEAD_LIGHT_RADIUS );
	headLight.shader = declManager->FindMaterial( "lights/defaultPointLight" );
	headLight.noShadows = true;
	headLight.shaderParms[SHADERPARM_RED] = headLight.shaderParms[SHADERPARM_GREEN] = headLight.shaderParms[SHADERPARM_BLUE] = brightness;
	headLight.shaderParms[SHADERPARM_ALPHA] = 1.0f;
	if ( headLightHandle == -1 ) {
		headLightHandle = world->AddLightDef( &headLight );
	} else {
		world->UpdateLightDef( headLightHandle, &headLight );
	}
}

void idEditorWorld::Draw( const idVec3 & origin, const idMat3 & axis, float fovX, float fovY, int timeMsec ) {
	if ( world == nullptr ) {
		return;
	}
	DrawMarkers( origin );
	UpdateHeadLight( origin );

	renderView_t view;
	memset( &view, 0, sizeof( view ) );
	view.vieworg = origin;
	view.viewaxis = axis;
	view.fov_x = fovX;
	view.fov_y = fovY;
	view.time[0] = timeMsec;
	view.time[1] = timeMsec;
	for ( float & parm : view.shaderParms ) {
		parm = 1.0f;
	}
	view.shaderParms[4] = 0.0f;
	view.shaderParms[5] = 0.0f;
	view.shaderParms[6] = 0.0f;
	view.shaderParms[7] = 0.0f;
	world->RenderScene( &view );
}
