/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company. 
Copyright (C) 2026 Dmitry Patsura

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").  

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#pragma hdrstop
#include "idlib/precompiled.h"

#include "dmap.h"

dmapGlobals_t	dmapGlobals;
dmapArena_t		dmapArena;

// the compile that runs, for its callbacks
static const compileOptions_t *	dmapOptions;
static bool						dmapCollisionMap;

/*
============
DmapError
============
*/
void DmapError( const char *fmt, ... ) {
	char	text[MAX_STRING_CHARS];
	va_list	argptr;

	va_start( argptr, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	common->Error( "%s", text );
	// not reached: common->Error throws
	throw idException( text );
}

/*
============
DmapPoll
============
*/
void DmapPoll() {
	if ( dmapOptions != nullptr && dmapOptions->cancelled && dmapOptions->cancelled() ) {
		throw dmapCancelled_t();
	}
}

/*
============
DmapReport
============
*/
void DmapReport( const char *stage, float fraction ) {
	if ( dmapOptions != nullptr && dmapOptions->progress ) {
		dmapOptions->progress( stage, fraction );
	}
}

/*
============
DmapProgress
============
*/
void DmapProgress( const char *stage, float fraction ) {
	DmapReport( stage, fraction );
	DmapPoll();
}

/*
============
ResetDmapGlobals
============
*/
static void ResetDmapGlobals() {
	dmapGlobals.mapFileBase[0] = '\0';
	dmapGlobals.dmapFile = nullptr;
	dmapGlobals.mapPlanes.Clear();
	dmapGlobals.num_entities = 0;
	dmapGlobals.uEntities = nullptr;
	dmapGlobals.entityNum = 0;
	dmapGlobals.verbose = false;
	dmapGlobals.noOptimize = false;
	dmapGlobals.verboseentities = false;
	dmapGlobals.noCurves = false;
	dmapGlobals.fullCarve = false;
	dmapGlobals.noModelBrushes = false;
	dmapGlobals.noTJunc = false;
	dmapGlobals.nomerge = false;
	dmapGlobals.noFlood = false;
	dmapGlobals.noClipSides = false;
}

/*
============
DmapFree

Ends a compile, whichever way it went: frees what the compiler did not free itself
(after an error that is nearly everything) and leaves the globals as before a compile.
============
*/
void DmapFree() {
	if ( dmapCollisionMap ) {
		// the error came out of the collision model manager
		collisionModelManager->FreeMap();
		dmapCollisionMap = false;
	}
	ResetOutput();

	// the windings belong to the heap; a side copied by SplitBrush shares its visible hull
	idList<idWinding *> windings;
	CollectBrushWindings( windings );
	CollectFaceWindings( windings );
	CollectPortalWindings( windings );
	std::sort( windings.Ptr(), windings.Ptr() + windings.Num() );
	for ( int i = 0 ; i < windings.Num() ; i++ ) {
		if ( i == 0 || windings[i] != windings[i - 1] ) {
			delete windings[i];
		}
	}

	dmapArena.Clear();
	ResetMapLoader();
	ResetTJunctionHash();
	ResetOptimizer();

	delete dmapGlobals.dmapFile;
	ResetDmapGlobals();
	dmapOptions = nullptr;
}

/*
============
ProcessModel
============
*/
static bool ProcessModel( uEntity_t *e, bool floodFill ) {
	// only the world is large enough to have stages worth showing
	const auto stage = [floodFill]( const char *name ) {
		if ( floodFill ) {
			DmapProgress( name, 0.0f );
		} else {
			DmapPoll();
		}
	};

	// build a bsp tree using all of the sides
	// of all of the structural brushes
	stage( "bsp" );
	bspface_t *faces = MakeStructuralBspFaceList ( e->primitives );
	e->tree = FaceBSP( faces );

	// create portals at every leaf intersection
	// to allow flood filling
	stage( "portals" );
	MakeTreePortals( e->tree );

	// classify the leafs as opaque or areaportal
	FilterBrushesIntoTree( e );

	// see if the bsp is completely enclosed
	if ( floodFill && !dmapGlobals.noFlood ) {
		stage( "flood" );
		if ( FloodEntities( e->tree ) ) {
			// set the outside leafs to opaque
			FillOutside( e );
		} else {
			common->Printf ( "**********************\n" );
			common->Warning( "******* leaked *******" );
			common->Printf ( "**********************\n" );
			LeakFile( e->tree );
			// bail out here.  If someone really wants to
			// process a map that leaks, they should use
			// -noFlood
			return false;
		}
	}

	// get minimum convex hulls for each visible side
	// this must be done before creating area portals,
	// because the visible hull is used as the portal
	stage( "clip sides" );
	ClipSidesByTree( e );

	// determine areas before clipping tris into the
	// tree, so tris will never cross area boundaries
	stage( "areas" );
	FloodAreas( e );

	// we now have a BSP tree with solid and non-solid leafs marked with areas
	// all primitives will now be clipped into this, throwing away
	// fragments in the solid areas
	stage( "primitives" );
	PutPrimitivesInAreas( e );

	// optimizing is a superset of fixing tjunctions
	stage( "optimize" );
	if ( !dmapGlobals.noOptimize ) {
		OptimizeEntity( e );
	} else  if ( !dmapGlobals.noTJunc ) {
		FixEntityTjunctions( e );
	}

	// now fix t junctions across areas
	stage( "t-junctions" );
	FixGlobalTjunctions( e );

	return true;
}

/*
============
ProcessModels
============
*/
static bool ProcessModels() {
	const bool oldVerbose = dmapGlobals.verbose;

	for ( dmapGlobals.entityNum = 0 ; dmapGlobals.entityNum < dmapGlobals.num_entities ; dmapGlobals.entityNum++ ) {

		uEntity_t &entity = dmapGlobals.uEntities[dmapGlobals.entityNum];
		if ( !entity.primitives ) {
			continue;
		}

		common->Printf( "############### entity %i ###############\n", dmapGlobals.entityNum );
		if ( dmapGlobals.entityNum > 0 ) {
			DmapProgress( "entities", static_cast<float>( dmapGlobals.entityNum ) / static_cast<float>( dmapGlobals.num_entities ) );
		}

		// if we leaked, stop without any more processing
		if ( !ProcessModel( &entity, dmapGlobals.entityNum == 0 ) ) {
			return false;
		}

		// we usually don't want to see output for submodels unless
		// something strange is going on
		if ( !dmapGlobals.verboseentities ) {
			dmapGlobals.verbose = false;
		}
	}

	dmapGlobals.verbose = oldVerbose;

	return true;
}

/*
============
DmapCompile

No shadow volumes are precomputed: the engine builds them when a .proc has none.
There is no AAS compiler either, so a compiled map has no navigation for monsters.
Leaves through an exception on an error or a cancel; DmapFree has to follow in any case.
============
*/
void DmapCompile( const compileOptions_t &options, compileResult_t &result ) {
	ResetDmapGlobals();
	dmapOptions = &options;

	common->Printf("---- dmap ----\n");

	// A new map has no resource file of its own, its media are in the other maps'. An image that
	// is not found now stays the default one for the game that loads the map next.
	fileSystem->MountAllMapResources();

	dmapGlobals.verbose = options.verbose;
	dmapGlobals.verboseentities = options.verboseEntities;
	dmapGlobals.noFlood = options.noFlood;
	dmapGlobals.noCurves = options.noCurves;
	dmapGlobals.noModelBrushes = options.noModelBrushes;
	dmapGlobals.noClipSides = options.noClipSides;
	dmapGlobals.fullCarve = !options.noCarve;
	dmapGlobals.noTJunc = options.noTJunc;
	// triangle optimization won't work properly without tjunction fixing
	dmapGlobals.noOptimize = options.noOptimize || options.noTJunc;

	idStr passedName = options.mapName;		// may have an extension
	passedName.BackSlashesToSlashes();
	if ( passedName.Icmpn( "maps/", 4 ) != 0 ) {
		passedName = "maps/" + passedName;
	}
	passedName.StripFileExtension();
	idStr::Copynz( dmapGlobals.mapFileBase, passedName, sizeof(dmapGlobals.mapFileBase) );

	// delete any old line leak file; only the one dmap wrote, never in the game install
	const idStr leakFile = passedName + ".lin";
	::remove( fileSystem->RelativePathToOSPath( leakFile, "fs_savepath" ) );

	//
	// start from scratch
	//
	int start = Sys_Milliseconds();

	DmapProgress( "load", 0.0f );
	if ( !LoadDMapFile( passedName ) ) {
		result.status = compileStatus_t::FAILED;
		sprintf( result.error, "Couldn't load map file: '%s'", passedName.c_str() );
		return;
	}

	if ( !ProcessModels() ) {
		result.status = compileStatus_t::LEAKED;
		result.leakFile = leakFile;
		return;
	}

	// no cancel from here on: a new .proc with the old collision map would be worse than waiting
	DmapReport( "write", 0.0f );
	WriteOutputFile();
	result.areas = dmapGlobals.uEntities[0].numAreas;
	result.portals = numInterAreaPortals;
	result.triangles = NumOutputTriangles();

	FreeDMapFile();

	int end = Sys_Milliseconds();
	common->Printf( "-----------------------\n" );
	common->Printf( "%5.0f seconds for dmap\n", ( end - start ) * 0.001f );

	if ( !options.noCM ) {
		// create the collision map
		DmapReport( "collision map", 0.0f );
		start = Sys_Milliseconds();

		dmapCollisionMap = true;
		collisionModelManager->LoadMap( dmapGlobals.dmapFile );
		collisionModelManager->FreeMap();
		dmapCollisionMap = false;

		end = Sys_Milliseconds();
		common->Printf( "-------------------------------------\n" );
		common->Printf( "%5.0f seconds to create collision map\n", ( end - start ) * 0.001f );
	}

	result.status = compileStatus_t::OK;
}
