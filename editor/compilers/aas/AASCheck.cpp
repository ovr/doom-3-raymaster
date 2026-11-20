// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include "aas/AASFile_local.h"
#include "../MapCompiler.h"
#include "Brush.h"

/*
	aasCheck: the AAS compiler against the retail maps.

	  retail maps/<map>.aas48, ... ── Load ── counts ─┐
	                                                  ├─ close to each other, and both with the map's CRC
	  maps/<map>.map ── CompileMap ── CompileAAS ── Load ── counts ─┘

	The .proc is compiled first because the AAS compiler clips the brush sides with its BSP tree, as id's
	build did. Exact equality is not expected: the brush BSP depends on the last bit of every plane test.
*/

namespace {

constexpr int NUM_COUNTS = 7;
constexpr const char *countNames[NUM_COUNTS] = { "areas", "reachabilities", "clusters", "portals", "nodes", "edges", "faces" };

// a count passes within this fraction of the retail one, or within the absolute slack for small numbers
constexpr float countTolerance[NUM_COUNTS] = { 0.10f, 0.10f, 0.25f, 0.25f, 0.10f, 0.10f, 0.10f };
constexpr int countSlack[NUM_COUNTS] = { 10, 30, 4, 6, 20, 40, 20 };

struct aasCounts_t {
	bool	loaded = false;
	int		counts[NUM_COUNTS] = {};
};

void RemoveSaved( const char *relativePath ) {
	// not fileSystem->RemoveFile: that one also deletes in the game install
	::remove( fileSystem->RelativePathToOSPath( relativePath, "fs_savepath" ) );
}

// loads the file as the game does, with the CRC of the map's geometry
[[nodiscard]] aasCounts_t CountAAS( const idStr &fileName, unsigned int mapCRC ) {
	aasCounts_t counts;
	idAASFileLocal file;
	if ( !file.Load( fileName, mapCRC ) ) {
		return counts;
	}
	counts.loaded = true;
	counts.counts[0] = file.GetNumAreas();
	for ( int i = 0; i < file.GetNumAreas(); i++ ) {
		for ( const idReachability *reach = file.GetArea( i ).reach; reach != nullptr; reach = reach->next ) {
			counts.counts[1]++;
		}
	}
	counts.counts[2] = file.GetNumClusters();
	counts.counts[3] = file.GetNumPortals();
	counts.counts[4] = file.GetNumNodes();
	counts.counts[5] = file.GetNumEdges();
	counts.counts[6] = file.GetNumFaces();
	return counts;
}

[[nodiscard]] bool CountsClose( const aasCounts_t &compiled, const aasCounts_t &retail ) {
	for ( int i = 0; i < NUM_COUNTS; i++ ) {
		const int difference = abs( compiled.counts[i] - retail.counts[i] );
		if ( difference > countSlack[i] && static_cast<float>( difference ) > countTolerance[i] * static_cast<float>( retail.counts[i] ) ) {
			return false;
		}
	}
	return true;
}

}	// namespace

CONSOLE_COMMAND_SHIP( aasCheck, "compiles the AAS of the retail maps and compares it with their retail AAS files: aasCheck [part of a map name] [keep]", nullptr ) {
	if ( common->IsMapSpawned() ) {
		common->Printf( "aasCheck: a map is running, disconnect first\n" );
		return;
	}
	const char * const filter = ( args.Argc() > 1 && idStr::Icmp( args.Argv( 1 ), "keep" ) != 0 ) ? args.Argv( 1 ) : "";
	const bool keep = idStr::Icmp( args.Argv( args.Argc() - 1 ), "keep" ) == 0;

	// the retail files are in the maps' resource files; a file of the save path has to win over them
	fileSystem->MountAllMapResources();
	const int oldPriority = cvarSystem->GetCVarInteger( "fs_resourceLoadPriority" );
	cvarSystem->SetCVarInteger( "fs_resourceLoadPriority", 0 );

	idStrList types;
	const idDict *typesDict = gameEdit->FindEntityDefDict( "aas_types", false );
	for ( const idKeyValue *kv = ( typesDict != nullptr ) ? typesDict->MatchPrefix( "type" ) : nullptr; kv != nullptr; kv = typesDict->MatchPrefix( "type", kv ) ) {
		types.Append( kv->GetValue() );
	}

	int maps = 0;
	int files = 0;
	int failed = 0;
	int totals[2][NUM_COUNTS] = {};
	const int start = Sys_Milliseconds();
	const int numMaps = declManager->GetNumDecls( DECL_MAPDEF );
	for ( int i = 0; i < numMaps; i++ ) {
		const idStr name = declManager->DeclByIndex( DECL_MAPDEF, i, false )->GetName();
		if ( filter[0] != '\0' && name.Find( filter, false ) == -1 ) {
			continue;
		}
		const idStr base = idStr( "maps/" ) + name;
		// leftovers of an earlier run would be taken for the retail files
		RemoveSaved( base + ".proc" );
		RemoveSaved( idStr( "generated/maps/" ) + name + ".bproc" );
		for ( int t = 0; t < types.Num(); t++ ) {
			RemoveSaved( base + "." + types[t] );
		}
		if ( fileSystem->GetFileLength( base + ".map" ) <= 0 ) {
			continue;
		}

		idMapFile mapFile;
		if ( !mapFile.Parse( base + ".map" ) ) {
			continue;
		}
		const unsigned int mapCRC = mapFile.GetGeometryCRC();

		idList<aasCounts_t> retail;
		bool any = false;
		for ( int t = 0; t < types.Num(); t++ ) {
			const idStr fileName = base + "." + types[t];
			retail.Append( ( fileSystem->GetFileLength( fileName ) > 0 ) ? CountAAS( fileName, mapCRC ) : aasCounts_t() );
			any = any || retail[t].loaded;
		}
		if ( !any ) {
			continue;
		}
		maps++;

		compileOptions_t mapOptions;
		mapOptions.mapName = name;
		mapOptions.noCM = true;
		compileResult_t mapResult;
		(void)CompileMap( mapOptions, mapResult );

		aasCompileOptions_t options;
		options.mapName = name;
		aasCompileResult_t result;
		(void)CompileAAS( options, result );

		for ( int t = 0; t < types.Num(); t++ ) {
			const idStr fileName = base + "." + types[t];
			bool written = false;
			for ( int f = 0; f < result.files.Num(); f++ ) {
				written = written || result.files[f].fileName.Icmp( fileName ) == 0;
			}
			if ( !retail[t].loaded && !written ) {
				continue;
			}
			files++;
			const aasCounts_t compiled = written ? CountAAS( fileName, mapCRC ) : aasCounts_t();
			const bool ok = retail[t].loaded && compiled.loaded && CountsClose( compiled, retail[t] );
			if ( !ok ) {
				failed++;
			}
			idStr line;
			for ( int c = 0; c < NUM_COUNTS; c++ ) {
				line += va( "%s%s %d/%d", ( c > 0 ) ? ", " : "", countNames[c], compiled.counts[c], retail[t].counts[c] );
				if ( retail[t].loaded && compiled.loaded ) {
					totals[0][c] += compiled.counts[c];
					totals[1][c] += retail[t].counts[c];
				}
			}
			common->Printf( "aasCheck %s.%s: %s, %.0f s; %s\n", name.c_str(), types[t].c_str(),
				ok ? "ok" : ( !compiled.loaded ? "NOT COMPILED" : ( !retail[t].loaded ? "NO RETAIL FILE" : "DIFFERENT" ) ), result.seconds, line.c_str() );
		}
		if ( result.status != compileStatus_t::OK ) {
			common->Printf( "aasCheck %s: the compile ended with status %d %s\n", name.c_str(), static_cast<int>( result.status ), result.error.c_str() );
		}

		if ( !keep ) {
			RemoveSaved( base + ".proc" );
			RemoveSaved( idStr( "generated/maps/" ) + name + ".bproc" );
			for ( int t = 0; t < types.Num(); t++ ) {
				RemoveSaved( base + "." + types[t] );
			}
		}
	}
	cvarSystem->SetCVarInteger( "fs_resourceLoadPriority", oldPriority );

	idStr line;
	for ( int c = 0; c < NUM_COUNTS; c++ ) {
		line += va( "%s%s %d/%d", ( c > 0 ) ? ", " : "", countNames[c], totals[0][c], totals[1][c] );
	}
	common->Printf( "aasCheck totals: %s\n", line.c_str() );
	common->Printf( "aasCheck: %d maps, %d files checked, %d failed, %d s\n", maps, files, failed, ( Sys_Milliseconds() - start ) / 1000 );
}

namespace {

struct aasAbortRun_t {
	aasCompileResult_t	result;
	unsigned int		checksum = 0;		// of all files written
	int					polls = 0;
	int					left = 0;			// compiler objects alive after the compile
};

// one AAS compile that is cancelled at its cancelAt-th poll, 0 for never
[[nodiscard]] aasAbortRun_t RunCancellable( const idStr &name, int cancelAt ) {
	aasAbortRun_t run;
	aasCompileOptions_t options;
	options.mapName = name;
	options.cancelled = [&run, cancelAt]() {
		run.polls++;
		return run.polls == cancelAt;
	};
	(void)CompileAAS( options, run.result );
	run.left = idAASCompileObject::NumLive();
	for ( int i = 0; i < run.result.files.Num(); i++ ) {
		idFile * const file = fileSystem->OpenExplicitFileRead( fileSystem->RelativePathToOSPath( run.result.files[i].fileName, "fs_savepath" ) );
		if ( file == nullptr ) {
			continue;
		}
		idTempArray<byte> data( file->Length() );
		file->Read( data.Ptr(), file->Length() );
		run.checksum = run.checksum * 31 + MD5_BlockChecksum( data.Ptr(), file->Length() );
		fileSystem->CloseFile( file );
	}
	return run;
}

}	// namespace

CONSOLE_COMMAND_SHIP( aasCheckAbort, "cancels an AAS compile at several places; the compiles after them must give the same files: aasCheckAbort <map>", nullptr ) {
	if ( args.Argc() < 2 || common->IsMapSpawned() ) {
		common->Printf( "usage: aasCheckAbort <map>, without a running map\n" );
		return;
	}
	const idStr name = args.Argv( 1 );

	const aasAbortRun_t clean = RunCancellable( name, 0 );
	if ( clean.result.status != compileStatus_t::OK || clean.result.files.Num() == 0 ) {
		common->Printf( "aasCheckAbort %s: FAILED, the map gives no AAS file\n", name.c_str() );
		return;
	}

	bool ok = true;
	for ( const int cancelAt : { 2, clean.polls / 4, clean.polls / 2, clean.polls - 1 } ) {
		const aasAbortRun_t cancelled = RunCancellable( name, cancelAt );
		const aasAbortRun_t after = RunCancellable( name, 0 );
		const bool same = cancelled.result.status == compileStatus_t::CANCELLED && cancelled.left == 0
			&& after.result.status == compileStatus_t::OK && after.checksum == clean.checksum && after.polls == clean.polls && after.left == 0;
		ok = ok && same;
		common->Printf( "aasCheckAbort %s: cancel at poll %d of %d: %s; status %d, objects left %d; the compile after it: status %d, %d files, checksum %08x/%08x\n",
			name.c_str(), cancelAt, clean.polls, same ? "ok" : "DIFFERENT", static_cast<int>( cancelled.result.status ), cancelled.left,
			static_cast<int>( after.result.status ), after.result.files.Num(), after.checksum, clean.checksum );
	}
	common->Printf( "aasCheckAbort %s: %s, %d files\n", name.c_str(), ok ? "ok" : "FAILED", clean.result.files.Num() );
}
