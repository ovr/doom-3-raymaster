// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

/*
	editorCheckMaps: the proof that the editor can save a map without breaking it.

	The compiled files of a map (.cm, .aas, .bproc) are tied to the CRC of its geometry, and the
	game rejects them when the .map no longer matches. So every map the game data declares is read
	as the editor reads it, written out and read again: the geometry CRC, the entities, their
	key/values and their primitive counts must come back the same.
*/

namespace {

constexpr char		CHECK_FILE[] = "maps/_editorcheck";

// "" when the two maps hold the same thing
[[nodiscard]] idStr CompareMaps( const idMapFile & original, const idMapFile & written ) {
	if ( original.GetNumEntities() != written.GetNumEntities() ) {
		return va( "%d entities became %d", original.GetNumEntities(), written.GetNumEntities() );
	}
	for ( int i = 0; i < original.GetNumEntities(); i++ ) {
		const idMapEntity & a = *original.GetEntity( i );
		const idMapEntity & b = *written.GetEntity( i );
		if ( a.GetNumPrimitives() != b.GetNumPrimitives() ) {
			return va( "entity %d: %d primitives became %d", i, a.GetNumPrimitives(), b.GetNumPrimitives() );
		}
		if ( a.epairs.GetNumKeyVals() != b.epairs.GetNumKeyVals() ) {
			return va( "entity %d: %d keys became %d", i, a.epairs.GetNumKeyVals(), b.epairs.GetNumKeyVals() );
		}
		for ( int k = 0; k < a.epairs.GetNumKeyVals(); k++ ) {
			const idKeyValue & key = *a.epairs.GetKeyVal( k );
			const idKeyValue * const other = b.epairs.FindKey( key.GetKey() );
			if ( other == nullptr || other->GetValue().Cmp( key.GetValue() ) != 0 ) {
				return va( "entity %d: key \"%s\" changed", i, key.GetKey().c_str() );
			}
		}
		if ( a.GetGeometryCRC() != b.GetGeometryCRC() ) {
			return va( "entity %d: the geometry CRC changed", i );
		}
	}
	if ( original.GetGeometryCRC() != written.GetGeometryCRC() ) {
		return "the geometry CRC of the map changed";
	}
	return "";
}

}	// namespace

CONSOLE_COMMAND_SHIP( editorCheckMaps, "reads, writes and reads again every map (or those whose name contains the argument) and compares", nullptr ) {
	const char * const filter = ( args.Argc() > 1 ) ? args.Argv( 1 ) : "";
	int numChecked = 0;
	int numFailed = 0;
	int numMissing = 0;

	const int numMaps = declManager->GetNumDecls( DECL_MAPDEF );
	for ( int i = 0; i < numMaps; i++ ) {
		const idDecl * const mapDef = declManager->DeclByIndex( DECL_MAPDEF, i, false );
		const idStr name = mapDef->GetName();
		if ( filter[0] != '\0' && name.Find( filter, false ) == -1 ) {
			continue;
		}
		const idStr fileName = idStr( "maps/" ) + name;

		// the .map is in the map's own resource file
		fileSystem->BeginLevelLoad( name, nullptr, 0 );
		idMapFile original;
		const bool found = original.Parse( fileName, true, false, true );
		fileSystem->EndLevelLoad();
		fileSystem->UnloadMapResources( name );
		if ( !found ) {
			numMissing++;
			continue;
		}

		idStr problem;
		idMapFile written;
		if ( !original.Write( CHECK_FILE, ".map" ) ) {
			problem = "could not be written";
		} else if ( !written.Parse( CHECK_FILE, true, false, true ) ) {
			problem = "the written file could not be read";
		} else {
			problem = CompareMaps( original, written );
		}

		numChecked++;
		if ( problem.IsEmpty() ) {
			common->Printf( "ok      %s (%d entities, CRC %08x)\n", name.c_str(), original.GetNumEntities(), original.GetGeometryCRC() );
		} else {
			numFailed++;
			common->Printf( "FAILED  %s: %s\n", name.c_str(), problem.c_str() );
		}
	}
	fileSystem->RemoveFile( va( "%s.map", CHECK_FILE ) );

	common->Printf( "editorCheckMaps: %d maps checked, %d failed, %d without a .map file\n", numChecked, numFailed, numMissing );
}
