// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include "MapCompiler.h"
#include "dmap/dmap.h"

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

namespace {

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
