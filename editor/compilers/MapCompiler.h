// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#ifndef __MAPCOMPILER_H__
#define __MAPCOMPILER_H__

#include <functional>

/*
===============================================================================

	The map compiler as one call.

	  maps/<map>.map ── CompileMap ──┬── maps/<map>.proc, maps/<map>.cm
	                                 └── maps/<map>.lin when the map leaks

	Everything is written into fs_savepath. Whatever the outcome, the compiler
	holds no memory and no state afterwards: an error inside it comes back as
	compileStatus_t::FAILED, it does not unwind through the caller.

	Main thread only, and never while a game map runs.

===============================================================================
*/

struct compileOptions_t {
	idStr				mapName;					// "editor/box1", with or without "maps/" and an extension

	bool				noCM = false;				// no collision map
	bool				noFlood = false;			// a leak is not an error: the outside is kept
	bool				noOptimize = false;
	bool				noTJunc = false;			// implies noOptimize
	bool				noCurves = false;
	bool				noModelBrushes = false;
	bool				noClipSides = false;
	bool				noCarve = false;
	bool				verbose = false;
	bool				verboseEntities = false;

	// the stage that runs now and how far it is, 0 to 1
	std::function<void( const char *stage, float fraction )>	progress;
	// true ends the compile; asked at every stage and inside the long loops, so it has to be cheap
	std::function<bool()>										cancelled;
};

enum class compileStatus_t {
	OK,
	LEAKED,				// leakFile was written, nothing else
	CANCELLED,
	FAILED				// see error
};

struct compileResult_t {
	compileStatus_t		status = compileStatus_t::FAILED;
	idStr				error;
	idStr				leakFile;					// relative to fs_savepath
	int					areas = 0;					// of the world
	int					portals = 0;				// between the areas
	int					triangles = 0;				// written, of all models
	float				seconds = 0.0f;
};

// true when the map was compiled
bool CompileMap( const compileOptions_t &options, compileResult_t &result );

#endif /* !__MAPCOMPILER_H__ */
