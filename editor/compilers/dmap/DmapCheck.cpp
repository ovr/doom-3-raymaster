// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "idlib/precompiled.h"

#include "renderer/tr_local.h"
#include "dmap.h"

/*
	dmapCheck: the compiler against the retail maps.

	  retail generated/maps/<map>.bproc ── InitFromMap ── counts ─┐
	                                                              ├─ areas, portals and models must be equal
	  maps/<map>.map ── Dmap ── maps/<map>.proc ── InitFromMap ── counts ─┘

	The BSP nodes, surfaces, vertices and triangles are printed with their difference: the optimizer of this
	build and of the one id used need not agree to the last triangle.
*/

namespace {

struct checkVert_t {
	idVec3	xyz;
	idVec2	st;
	idVec3	normal;
	idVec3	tangent;
	idVec3	bitangent;
};

struct surfaceVerts_t {
	idStr				name;		// model/material
	idList<checkVert_t>	verts;
};

struct worldCounts_t {
	int		areas = 0;
	int		portals = 0;
	int		nodes = 0;
	int		models = 0;
	int		surfaces = 0;
	int		verts = 0;
	int		triangles = 0;
	int		zeroArea = 0;		// triangles without an area, their corners on one line
	int		duplicates = 0;		// triangles a surface holds more than once
	bool	loaded = false;
	idStrList	modelNames;
	idList<int>	modelTriangles;
	// with a filter: every surface's vertices, for CompareNormals
	idList<surfaceVerts_t>	surfaceVerts;
};

void CountBadTriangles( const srfTriangles_t & tri, int & zeroArea, int & duplicates ) {
	constexpr float ZERO_AREA = 0.001f;
	if ( tri.verts == nullptr || tri.indexes == nullptr ) {
		return;
	}
	// the three indexes, rotated to start with the smallest: the same triangle gives the same key
	idList<uint64> keys;
	keys.Resize( tri.numIndexes / 3 + 1 );
	for ( int i = 0; i + 2 < tri.numIndexes; i += 3 ) {
		const triIndex_t * const index = tri.indexes + i;
		if ( idWinding::TriangleArea( tri.verts[index[0]].xyz, tri.verts[index[1]].xyz, tri.verts[index[2]].xyz ) < ZERO_AREA ) {
			zeroArea++;
		}
		const int first = ( index[0] <= index[1] && index[0] <= index[2] ) ? 0 : ( ( index[1] <= index[2] ) ? 1 : 2 );
		keys.Append( ( static_cast<uint64>( index[first] ) << 42 ) | ( static_cast<uint64>( index[( first + 1 ) % 3] ) << 21 ) | index[( first + 2 ) % 3] );
	}
	std::sort( keys.Ptr(), keys.Ptr() + keys.Num() );
	duplicates += static_cast<int>( keys.Ptr() + keys.Num() - std::unique( keys.Ptr(), keys.Ptr() + keys.Num() ) );
}

[[nodiscard]] worldCounts_t CountWorld( const char * mapFileName, bool keepVerts ) {
	worldCounts_t counts;
	idRenderWorldLocal * const world = static_cast<idRenderWorldLocal *>( renderSystem->AllocRenderWorld() );
	if ( world->InitFromMap( mapFileName ) ) {
		counts.loaded = true;
		counts.areas = world->numPortalAreas;
		counts.portals = world->numInterAreaPortals;
		counts.nodes = world->numAreaNodes;
		for ( int i = 0; i < world->localModels.Num(); i++ ) {
			const idRenderModel * const model = world->localModels[i];
			// the shadow volumes id's dmap precomputed; this one makes none
			if ( idStr::Icmpn( model->Name(), "_prelight_", 10 ) == 0 ) {
				continue;
			}
			counts.models++;
			const int before = counts.triangles;
			for ( int j = 0; j < model->NumSurfaces(); j++ ) {
				const srfTriangles_t * const tri = model->Surface( j )->geometry;
				counts.surfaces++;
				if ( tri != nullptr ) {
					counts.verts += tri->numVerts;
					counts.triangles += tri->numIndexes / 3;
					CountBadTriangles( *tri, counts.zeroArea, counts.duplicates );
				}
				if ( keepVerts && tri != nullptr && tri->verts != nullptr ) {
					surfaceVerts_t & surface = counts.surfaceVerts.Alloc();
					surface.name = idStr( model->Name() ) + "/" + model->Surface( j )->shader->GetName();
					surface.verts.SetNum( tri->numVerts );
					for ( int v = 0; v < tri->numVerts; v++ ) {
						surface.verts[v] = { tri->verts[v].xyz, tri->verts[v].GetTexCoord(), tri->verts[v].GetNormal(), tri->verts[v].GetTangent(), tri->verts[v].GetBiTangent() };
					}
				}
			}
			counts.modelNames.Append( model->Name() );
			counts.modelTriangles.Append( counts.triangles - before );
		}
	}
	renderSystem->FreeRenderWorld( world );
	return counts;
}

void RemoveSaved( const char * relativePath ) {
	// not fileSystem->RemoveFile: that one also deletes in the game install
	::remove( fileSystem->RelativePathToOSPath( relativePath, "fs_savepath" ) );
}

[[nodiscard]] float AngleBetween( const idVec3 & a, const idVec3 & b ) {
	return RAD2DEG( idMath::ACos( idMath::ClampFloat( -1.0f, 1.0f, a * b ) ) );
}

// The loader moves the texture coordinates of an island by whole numbers, so only the fractions compare.
[[nodiscard]] bool SameTexCoord( const idVec2 & a, const idVec2 & b ) {
	const float ds = a.x - b.x;
	const float dt = a.y - b.y;
	return idMath::Fabs( ds - idMath::Rint( ds ) ) < 0.02f && idMath::Fabs( dt - idMath::Rint( dt ) ) < 0.02f;
}

[[nodiscard]] int PositionKey( const idVec3 & v ) {
	return ( ( idMath::Ftoi( v.x ) * 73856093 ) ^ ( idMath::Ftoi( v.y ) * 19349663 ) ^ ( idMath::Ftoi( v.z ) * 83492791 ) ) & 4095;
}

// Per surface, the vertices of the compiled world against the retail ones at the same place and
// texture coordinate: the average and the largest angle between their normals and tangents.
void CompareNormals( const worldCounts_t & retail, const worldCounts_t & compiled ) {
	constexpr int	MAX_PRINTED = 20;
	constexpr float	MIN_PRINTED_ANGLE = 1.0f;
	constexpr float	APART_ANGLE = 20.0f;
	struct result_t {
		idStr	name;
		int		verts = 0;
		int		matched = 0;
		float	normalAverage = 0.0f;
		float	normalMax = 0.0f;
		float	tangentAverage = 0.0f;
		int		apart = 0;			// vertices whose normal or tangent is off by more than APART_ANGLE
		float	score = 0.0f;		// the larger average; above 180 with vertices apart
		bool	printed = false;
	};
	idList<result_t> results;
	idList<bool> used;
	used.SetNum( retail.surfaceVerts.Num() );
	for ( int i = 0; i < used.Num(); i++ ) {
		used[i] = false;
	}
	int totalVerts = 0;
	int totalMatched = 0;
	double totalAngle = 0.0;
	for ( int i = 0; i < compiled.surfaceVerts.Num(); i++ ) {
		const surfaceVerts_t & ours = compiled.surfaceVerts[i];
		int other = -1;
		for ( int j = 0; j < retail.surfaceVerts.Num(); j++ ) {
			if ( !used[j] && retail.surfaceVerts[j].name == ours.name ) {
				other = j;
				break;
			}
		}
		if ( other == -1 ) {
			continue;
		}
		used[other] = true;
		const surfaceVerts_t & theirs = retail.surfaceVerts[other];
		idHashIndex hash( 4096, theirs.verts.Num() );
		for ( int v = 0; v < theirs.verts.Num(); v++ ) {
			hash.Add( PositionKey( theirs.verts[v].xyz ), v );
		}
		result_t result;
		result.name = ours.name;
		result.verts = ours.verts.Num();
		float normalSum = 0.0f;
		float tangentSum = 0.0f;
		for ( int v = 0; v < ours.verts.Num(); v++ ) {
			const checkVert_t & a = ours.verts[v];
			float best = -1.0f;
			float bestTangent = 0.0f;
			// a coordinate next to a whole number may be truncated to the other side
			for ( int n = 0; n < 8; n++ ) {
				const idVec3 probe( a.xyz.x + ( ( n & 1 ) ? 0.05f : -0.05f ), a.xyz.y + ( ( n & 2 ) ? 0.05f : -0.05f ), a.xyz.z + ( ( n & 4 ) ? 0.05f : -0.05f ) );
				for ( int h = hash.First( PositionKey( probe ) ); h != -1; h = hash.Next( h ) ) {
					const checkVert_t & b = theirs.verts[h];
					if ( !a.xyz.Compare( b.xyz, 0.05f ) || !SameTexCoord( a.st, b.st ) ) {
						continue;
					}
					// several vertices share a place and a coordinate at a mirror seam: the closest one
					const float angle = AngleBetween( a.normal, b.normal );
					const float tangentAngle = Max( AngleBetween( a.tangent, b.tangent ), AngleBetween( a.bitangent, b.bitangent ) );
					if ( best < 0.0f || angle + tangentAngle < best + bestTangent ) {
						best = angle;
						bestTangent = tangentAngle;
					}
				}
			}
			if ( best >= 0.0f ) {
				result.matched++;
				normalSum += best;
				tangentSum += bestTangent;
				result.normalMax = Max( result.normalMax, best );
				if ( best > APART_ANGLE || bestTangent > APART_ANGLE ) {
					result.apart++;
				}
			}
		}
		totalVerts += result.verts;
		totalMatched += result.matched;
		totalAngle += normalSum;
		if ( result.matched > 0 ) {
			result.normalAverage = normalSum / static_cast<float>( result.matched );
			result.tangentAverage = tangentSum / static_cast<float>( result.matched );
		}
		result.score = ( result.apart > 0 ) ? 180.0f + result.apart : Max( result.normalAverage, result.tangentAverage );
		results.Append( result );
	}
	common->Printf( "  normals: %d of %d vertices found in the retail world, average angle %.3f degrees\n",
		totalMatched, totalVerts, ( totalMatched > 0 ) ? totalAngle / totalMatched : 0.0 );
	for ( int n = 0; n < MAX_PRINTED; n++ ) {
		result_t * worst = nullptr;
		for ( int i = 0; i < results.Num(); i++ ) {
			result_t & result = results[i];
			if ( !result.printed && result.score >= MIN_PRINTED_ANGLE && ( worst == nullptr || result.score > worst->score ) ) {
				worst = &result;
			}
		}
		if ( worst == nullptr ) {
			break;
		}
		worst->printed = true;
		common->Printf( "  %s: normals %.2f average, %.2f largest, tangents %.2f average, %d apart; %d of %d vertices\n",
			worst->name.c_str(), worst->normalAverage, worst->normalMax, worst->tangentAverage, worst->apart, worst->matched, worst->verts );
	}
}

[[nodiscard]] float Percent( int now, int before ) {
	return ( before != 0 ) ? 100.0f * static_cast<float>( now - before ) / static_cast<float>( before ) : 0.0f;
}

}	// namespace

CONSOLE_COMMAND_SHIP( dmapCheck, "compiles the retail maps and compares the result with their retail .bproc: dmapCheck [part of a map name] [keep]", nullptr ) {
	if ( common->IsMapSpawned() ) {
		common->Printf( "dmapCheck: a map is running, disconnect first\n" );
		return;
	}
	const char * const filter = ( args.Argc() > 1 && idStr::Icmp( args.Argv( 1 ), "keep" ) != 0 ) ? args.Argv( 1 ) : "";
	const bool keep = idStr::Icmp( args.Argv( args.Argc() - 1 ), "keep" ) == 0;

	// the retail .map and .bproc files are in the maps' resource files
	fileSystem->MountAllMapResources();

	int checked = 0;
	int failed = 0;
	int skipped = 0;
	const int start = Sys_Milliseconds();
	const int numMaps = declManager->GetNumDecls( DECL_MAPDEF );
	for ( int i = 0; i < numMaps; i++ ) {
		const idStr name = declManager->DeclByIndex( DECL_MAPDEF, i, false )->GetName();
		if ( filter[0] != '\0' && name.Find( filter, false ) == -1 ) {
			continue;
		}
		const idStr mapFileName = idStr( "maps/" ) + name + ".map";
		const idStr procName = idStr( "maps/" ) + name + ".proc";
		const idStr bprocName = idStr( "generated/maps/" ) + name + ".bproc";
		// leftovers of an earlier run would be taken for the retail world
		RemoveSaved( procName );
		RemoveSaved( bprocName );
		if ( fileSystem->GetFileLength( mapFileName ) <= 0 ) {
			skipped++;
			continue;
		}

		renderSystem->BeginLevelLoad();
		const worldCounts_t retail = CountWorld( mapFileName, filter[0] != 0 );
		if ( !retail.loaded ) {
			renderSystem->EndLevelLoad();
			skipped++;
			continue;
		}

		const int mapStart = Sys_Milliseconds();
		idCmdArgs dmapArgs;
		dmapArgs.AppendArg( "dmap" );
		dmapArgs.AppendArg( "noCM" );
		dmapArgs.AppendArg( name );
		Dmap( dmapArgs );
		const int seconds = ( Sys_Milliseconds() - mapStart + 500 ) / 1000;

		const worldCounts_t compiled = CountWorld( mapFileName, filter[0] != 0 );
		renderSystem->EndLevelLoad();
		if ( !keep ) {
			RemoveSaved( procName );
			RemoveSaved( bprocName );
		}

		checked++;
		const bool same = compiled.loaded && compiled.areas == retail.areas && compiled.portals == retail.portals && compiled.models == retail.models;
		if ( !same ) {
			failed++;
		}
		common->Printf( "dmapCheck %s: %s, %d s; areas %d/%d, portals %d/%d, nodes %d/%d, models %d/%d, surfaces %d/%d, verts %d/%d (%+.2f%%), triangles %d/%d (%+.2f%%), without area %d/%d, duplicated %d/%d\n",
			name.c_str(), same ? "ok" : ( compiled.loaded ? "DIFFERENT" : "NOT COMPILED" ), seconds,
			compiled.areas, retail.areas, compiled.portals, retail.portals, compiled.nodes, retail.nodes, compiled.models, retail.models,
			compiled.surfaces, retail.surfaces, compiled.verts, retail.verts, Percent( compiled.verts, retail.verts ),
			compiled.triangles, retail.triangles, Percent( compiled.triangles, retail.triangles ),
			compiled.zeroArea, retail.zeroArea, compiled.duplicates, retail.duplicates );
		// with a filter, the models that differ most say where to look
		for ( int m = 0; filter[0] != 0 && m < retail.modelNames.Num() && m < compiled.modelNames.Num(); m++ ) {
			if ( abs( retail.modelTriangles[m] - compiled.modelTriangles[m] ) > 50 ) {
				common->Printf( "  %s / %s: triangles %d/%d\n", compiled.modelNames[m].c_str(), retail.modelNames[m].c_str(), compiled.modelTriangles[m], retail.modelTriangles[m] );
			}
		}
		if ( filter[0] != 0 && compiled.loaded ) {
			CompareNormals( retail, compiled );
		}
	}
	common->Printf( "dmapCheck: %d maps checked, %d failed, %d without a .map or a retail .bproc, %d s\n", checked, failed, skipped, ( Sys_Milliseconds() - start ) / 1000 );
}
