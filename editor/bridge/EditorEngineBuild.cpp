// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include "../compilers/MapCompiler.h"
#include "EditorEngineLocal.h"

/*
	Build: the document becomes what the game loads.

	  save ── CompileMap ──┬─ OK ───── CompileAAS (a map with monsters, not "fast")
	                       │              └── ShowCompiled: the .proc is the viewport's world again
	                       ├─ LEAKED ── the .lin is loaded and drawn; the preview stays
	                       └─ FAILED, CANCELLED ── the preview stays

	The compile blocks the frame it runs in. Its progress and its cancel check pump the UI's
	events, so the windows repaint and CancelBuild() can be called from there.
*/

namespace {

constexpr int		BUILD_PUMP_MSEC = 30;
const char * const	STATUS_NAMES[] = { "OK", "LEAKED", "CANCELLED", "FAILED" };

[[nodiscard]] int BuildStatus( compileStatus_t status ) {
	switch ( status ) {
		case compileStatus_t::OK:			return EDITOR_BUILD_OK;
		case compileStatus_t::LEAKED:		return EDITOR_BUILD_LEAKED;
		case compileStatus_t::CANCELLED:	return EDITOR_BUILD_CANCELLED;
		default:							return EDITOR_BUILD_FAILED;
	}
}

}	// namespace

void idEditorEngineLocal::PumpBuild() {
	const int now = Sys_Milliseconds();
	if ( now - lastBuildPumpMsec >= BUILD_PUMP_MSEC ) {
		lastBuildPumpMsec = now;
		ui->PumpEvents();
	}
}

void idEditorEngineLocal::BuildProgress( const char * part, const char * stage, float fraction ) {
	ui->OnBuildProgress( va( "%s: %s", part, stage ), fraction );
	PumpBuild();
}

void idEditorEngineLocal::ShowLeak( bool show ) {
	world.ShowLeak( show );
	if ( started ) {
		ui->OnDocumentChanged();
	}
}

bool idEditorEngineLocal::BuildNow( bool fast, bool cancelTest ) {
	if ( !world.IsOpen() || playing || building || tool.Dragging() || common->IsMapSpawned() ) {
		common->Printf( world.IsOpen() ? "editorBuild: not while the game plays or a build runs\n" : "No map is open in the editor.\n" );
		return false;
	}
	// the compilers read the file
	SaveMapNow();
	if ( history.IsDirty() || world.IsUnsaved() ) {
		return false;
	}
	common->Printf( "--------- Editor: build %s ---------\n", world.Name() );
	building = true;
	buildCancel = false;
	ui->OnDocumentChanged();

	compileOptions_t options;
	options.mapName = world.Name();
	options.progress = [this, cancelTest]( const char * stage, float fraction ) {
		BuildProgress( "map", stage, fraction );
		buildCancel |= cancelTest;
	};
	options.cancelled = [this] { PumpBuild(); return buildCancel; };
	compileResult_t compiled;
	CompileMap( options, compiled );

	idStr error = compiled.error;
	editorBuildResult_t result = {};
	result.status = BuildStatus( compiled.status );
	result.areas = compiled.areas;
	result.portals = compiled.portals;
	result.triangles = compiled.triangles;
	result.seconds = compiled.seconds;

	if ( compiled.status == compileStatus_t::OK ) {
		// a monster's entityDef names the AAS it walks on
		bool monsters = false;
		for ( int i = 1; i < world.NumEntities() && !monsters; i++ ) {
			monsters = !world.Entity( i ).removed && world.Entity( i ).args.GetString( "use_aas" )[0] != '\0';
		}
		if ( monsters && !fast ) {
			aasCompileOptions_t aasOptions;
			aasOptions.mapName = world.Name();
			aasOptions.progress = [this]( const char * stage, float fraction ) { BuildProgress( "aas", stage, fraction ); };
			aasOptions.cancelled = options.cancelled;
			aasCompileResult_t aas;
			CompileAAS( aasOptions, aas );
			result.seconds += aas.seconds;
			result.aasFiles = aas.files.Num();
			for ( int i = 0; i < aas.files.Num(); i++ ) {
				result.aasAreas += aas.files[i].areas;
			}
			if ( aas.status != compileStatus_t::OK ) {
				// the world is compiled all the same; monsters will not find their way
				result.status = BuildStatus( aas.status );
				error = va( "AAS: %s", ( aas.status == compileStatus_t::LEAKED ) ? "the map leaks for a monster's size" : aas.error.c_str() );
			}
		}
		world.ClearLeak();
		if ( !world.ShowCompiled() ) {
			result.status = EDITOR_BUILD_FAILED;
			error = "the compiled world could not be loaded";
		}
	} else if ( compiled.status == compileStatus_t::LEAKED ) {
		if ( !world.LoadLeak( compiled.leakFile ) ) {
			common->Warning( "Editor: couldn't read the leak line %s", compiled.leakFile.c_str() );
		}
		world.ShowLeak( true );
	}
	result.leakPoints = world.NumLeakPoints();
	result.error = error.c_str();

	building = false;
	common->Printf( "editorBuild: %s%s%s, %d areas, %d portals, %d triangles, %d AAS files (%d areas), %d leak points, %.1f seconds\n",
		STATUS_NAMES[result.status], error.IsEmpty() ? "" : " - ", error.c_str(), result.areas, result.portals, result.triangles,
		result.aasFiles, result.aasAreas, result.leakPoints, result.seconds );
	ui->OnBuildFinished( result );
	ui->OnDocumentChanged();
	return compiled.status == compileStatus_t::OK && !world.IsPreview();
}

CONSOLE_COMMAND_SHIP( editorBuild, "saves and compiles the map open in the editor; fast: no AAS for the monsters; cancel: a test, the build is cancelled at its first stage: editorBuild [fast|cancel]", nullptr ) {
	if ( Editor_NeedEngine() ) {
		editorEngine->BuildNow( idStr::Icmp( args.Argv( 1 ), "fast" ) == 0, idStr::Icmp( args.Argv( 1 ), "cancel" ) == 0 );
	}
}

CONSOLE_COMMAND_SHIP( editorBuildCancel, "ends the build that runs", nullptr ) {
	if ( Editor_NeedEngine() ) {
		editorEngine->CancelBuild();
	}
}

CONSOLE_COMMAND_SHIP( editorShowLeak, "shows or hides the leak line of the last build: editorShowLeak 0|1", nullptr ) {
	if ( args.Argc() != 2 || !Editor_NeedEngine() ) {
		common->Printf( "usage: editorShowLeak 0|1\n" );
		return;
	}
	editorEngine->ShowLeak( atoi( args.Argv( 1 ) ) != 0 );
}
