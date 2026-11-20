// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include "MapCompiler.h"
#include "dmap/dmap.h"
#include "aas/AASBuild_local.h"

bool CompileMap( const compileOptions_t &options, compileResult_t &result ) {
	result = compileResult_t();

	// the compiler and the collision model manager are engine globals a running game owns
	if ( common->IsMapSpawned() ) {
		result.error = "a map is running, disconnect first";
		return false;
	}

	const int start = Sys_Milliseconds();
	try {
		DmapCompile( options, result );
	} catch ( const dmapCancelled_t & ) {
		result.status = compileStatus_t::CANCELLED;
	} catch ( idException &e ) {
		// common->Error has printed it and dropped what the engine was doing
		result.status = compileStatus_t::FAILED;
		result.error = e.GetError();
	}
	DmapFree();

	if ( result.status != compileStatus_t::OK ) {
		result.areas = 0;
		result.portals = 0;
		result.triangles = 0;
	}
	result.seconds = static_cast<float>( Sys_Milliseconds() - start ) * 0.001f;
	return result.status == compileStatus_t::OK;
}

bool CompileAAS( const aasCompileOptions_t &options, aasCompileResult_t &result ) {
	result = aasCompileResult_t();

	// the declarations and the collision model manager are engine globals a running game owns
	if ( common->IsMapSpawned() ) {
		result.error = "a map is running, disconnect first";
		return false;
	}

	const int start = Sys_Milliseconds();
	try {
		AASCompile( options, result );
	} catch ( const aasCancelled_t & ) {
		result.status = compileStatus_t::CANCELLED;
	} catch ( idException &e ) {
		result.status = compileStatus_t::FAILED;
		result.error = e.GetError();
	}
	// id's code loses a few brushes and nodes on a compile that runs to its end too; they are freed here
	const int left = AASFree();
	if ( left != 0 ) {
		common->DPrintf( "CompileAAS: freed %d compiler objects that were left\n", left );
	}

	result.seconds = static_cast<float>( Sys_Milliseconds() - start ) * 0.001f;
	return result.status == compileStatus_t::OK;
}

namespace {

struct aasSwitch_t {
	const char *				name;
	bool aasCompileOptions_t::*	option;
};

constexpr aasSwitch_t aasSwitches[] = {
	{ "usePatches",		&aasCompileOptions_t::usePatches },
	{ "writeBrushMap",	&aasCompileOptions_t::writeBrushMap },
	{ "playerFlood",	&aasCompileOptions_t::playerFlood },
	{ "noOptimize",		&aasCompileOptions_t::noOptimize },
};

void CompileAASFromConsole( const idCmdArgs &args, bool reachabilityOnly ) {
	aasCompileOptions_t options;
	options.reachabilityOnly = reachabilityOnly;
	// the map name is the last argument, the switches come before it, with or without a dash
	for ( int i = 1; i < args.Argc() - 1; i++ ) {
		const char *s = args.Argv( i );
		if ( s[0] == '-' ) {
			s++;
		}
		for ( const aasSwitch_t &aasSwitch : aasSwitches ) {
			if ( idStr::Icmp( s, aasSwitch.name ) == 0 ) {
				common->Printf( "%s = true\n", aasSwitch.name );
				options.*aasSwitch.option = true;
			}
		}
	}
	if ( args.Argc() < 2 ) {
		common->Printf( "usage: %s [options] mapfile\n"
			"options: usePatches, writeBrushMap, playerFlood, noOptimize\n", args.Argv( 0 ) );
		return;
	}
	options.mapName = args.Argv( args.Argc() - 1 );

	common->ClearWarnings( "compiling AAS" );
	common->SetRefreshOnPrint( true );
	aasCompileResult_t result;
	(void)CompileAAS( options, result );
	common->SetRefreshOnPrint( false );
	common->PrintWarnings();

	switch ( result.status ) {
		case compileStatus_t::OK:
			for ( int i = 0; i < result.files.Num(); i++ ) {
				const aasFileResult_t &file = result.files[i];
				common->Printf( "%s: %s: %d areas, %d reachabilities, %d clusters, %d portals\n", args.Argv( 0 ), file.fileName.c_str(),
					file.areas, file.reachabilities, file.clusters, file.portals );
			}
			common->Printf( "%s: %d files, %.1f seconds\n", args.Argv( 0 ), result.files.Num(), result.seconds );
			break;
		case compileStatus_t::LEAKED:
			common->Printf( "%s: no entity that uses the AAS is inside the map, see %s\n", args.Argv( 0 ), result.leakFile.c_str() );
			break;
		case compileStatus_t::CANCELLED:
			common->Printf( "%s: cancelled\n", args.Argv( 0 ) );
			break;
		case compileStatus_t::FAILED:
			common->Printf( "%s: failed: %s\n", args.Argv( 0 ), result.error.c_str() );
			break;
	}
}

struct compileSwitch_t {
	const char *				name;
	bool compileOptions_t::*	option;
};

constexpr compileSwitch_t compileSwitches[] = {
	{ "v",					&compileOptions_t::verbose },
	{ "verboseentities",	&compileOptions_t::verboseEntities },
	{ "noFlood",			&compileOptions_t::noFlood },
	{ "noOpt",				&compileOptions_t::noOptimize },
	{ "noTjunc",			&compileOptions_t::noTJunc },
	{ "noCurves",			&compileOptions_t::noCurves },
	{ "noModels",			&compileOptions_t::noModelBrushes },
	{ "noClipSides",		&compileOptions_t::noClipSides },
	{ "noCarve",			&compileOptions_t::noCarve },
	{ "noCM",				&compileOptions_t::noCM },
};

// the switches come first, with or without a dash; false when no map name follows them
[[nodiscard]] bool ParseCompileArgs( const idCmdArgs &args, compileOptions_t &options ) {
	int i = 1;
	for ( ; i < args.Argc(); i++ ) {
		const char *s = args.Argv( i );
		if ( s[0] == '-' ) {
			s++;
			if ( s[0] == '\0' ) {
				continue;
			}
		}
		bool found = false;
		for ( const compileSwitch_t &compileSwitch : compileSwitches ) {
			if ( idStr::Icmp( s, compileSwitch.name ) == 0 ) {
				common->Printf( "%s = true\n", compileSwitch.name );
				options.*compileSwitch.option = true;
				found = true;
				break;
			}
		}
		if ( !found ) {
			break;
		}
	}
	if ( i >= args.Argc() ) {
		return false;
	}
	options.mapName = args.Argv( i );
	return true;
}

[[nodiscard]] bool CompileFromConsole( const compileOptions_t &options ) {
	common->ClearWarnings( "running dmap" );

	// refresh the screen each time we print so it doesn't look like it is hung
	common->SetRefreshOnPrint( true );
	compileResult_t result;
	const bool compiled = CompileMap( options, result );
	common->SetRefreshOnPrint( false );

	common->PrintWarnings();

	switch ( result.status ) {
		case compileStatus_t::OK:
			common->Printf( "dmap: %d areas, %d portals, %d triangles, %.1f seconds\n", result.areas, result.portals, result.triangles, result.seconds );
			break;
		case compileStatus_t::LEAKED:
			common->Printf( "dmap: the map leaks, see %s\n", result.leakFile.c_str() );
			break;
		case compileStatus_t::CANCELLED:
			common->Printf( "dmap: cancelled\n" );
			break;
		case compileStatus_t::FAILED:
			common->Printf( "dmap: failed: %s\n", result.error.c_str() );
			break;
	}
	return compiled;
}

}	// namespace

CONSOLE_COMMAND_SHIP( dmap, "compiles a map: maps/<map>.map into the .proc and the collision map", idCmdSystem::ArgCompletion_MapName ) {
	compileOptions_t options;
	if ( !ParseCompileArgs( args, options ) ) {
		common->Printf( "usage: dmap [options] mapfile\n"
			"options: noCM, noFlood, noOpt, noTjunc, noCurves, noModels, noClipSides, noCarve, v, verboseentities\n" );
		return;
	}
	(void)CompileFromConsole( options );
}

CONSOLE_COMMAND_SHIP( runAAS, "compiles the AAS files of a map: maps/<map>.map into maps/<map>.aas48, ...", idCmdSystem::ArgCompletion_MapName ) {
	CompileAASFromConsole( args, false );
}

CONSOLE_COMMAND_SHIP( runReach, "calculates the reachabilities and clusters of a map's AAS files again", idCmdSystem::ArgCompletion_MapName ) {
	CompileAASFromConsole( args, true );
}

CONSOLE_COMMAND_SHIP( testmap, "compiles a map and loads it", idCmdSystem::ArgCompletion_MapName ) {
	idStr map = args.Argv( 1 );
	if ( map.IsEmpty() ) {
		return;
	}
	map.StripFileExtension();

	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "disconnect" );

	compileOptions_t options;
	options.mapName = map;
	if ( CompileFromConsole( options ) ) {
		cmdSystem->BufferCommandText( CMD_EXEC_NOW, va( "devmap %s", map.c_str() ) );
	}
}
