/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Dmitry Patsura

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").  

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Backend.h"

idCVar r_d3d12Stats( "r_d3d12Stats", "0", CVAR_RENDERER | CVAR_BOOL,
	"1 = print the draws, PSOs, depth biases, buffer memory, uploads, submits, waits, barriers and GPU time per frame of Direct3D 12 once a second, and show them under com_showFPS" );
idCVar r_d3d12ShowImage( "r_d3d12ShowImage", "", CVAR_RENDERER,
	"the name of a texture (case-insensitive) Direct3D 12 draws over every frame: RGB and alpha panels, a cube as a cross of its faces" );
idCVar r_d3d12CheckImages( "r_d3d12CheckImages", "0", CVAR_RENDERER | CVAR_BOOL,
	"1 = Direct3D 12 hashes every texture upload and runs d3d12_checkImages after each level load" );

namespace {

constexpr int REPORT_MSEC = 1000;

// The commands reach the backend through this; Init sets it, Shutdown clears it.
idD3D12Context *	activeContext = nullptr;

[[nodiscard]] constexpr double Megabytes( uint64 bytes ) {
	return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 );
}

[[nodiscard]] constexpr double Kilobytes( uint64 bytes ) {
	return static_cast<double>( bytes ) / 1024.0;
}

[[nodiscard]] constexpr double Milliseconds( uint64 microseconds ) {
	return static_cast<double>( microseconds ) / 1000.0;
}

}	// namespace

void D3D12_SetActiveContext( idD3D12Context * context ) {
	activeContext = context;
}

CONSOLE_COMMAND( d3d12_checkImages, "reads every texture upload recorded under r_d3d12CheckImages 1 back from the GPU and compares it", nullptr ) {
	if ( activeContext == nullptr ) {
		common->Printf( "d3d12_checkImages: Direct3D 12 is not running\n" );
		return;
	}
	if ( !r_d3d12CheckImages.GetBool() ) {
		common->Printf( "d3d12_checkImages: uploads are hashed only with r_d3d12CheckImages 1; reload the map after setting it\n" );
	}
	static_cast<void>( activeContext->CheckTextures() );
}

CONSOLE_COMMAND( d3d12_compileAll, "emits and compiles every renderprogs/*.vertex and *.pixel for Direct3D 12 and prints the errors; 'd3d12_compileAll v' lists each source", nullptr ) {
	if ( activeContext == nullptr ) {
		common->Printf( "d3d12_compileAll: Direct3D 12 is not running\n" );
		return;
	}
	const bool verbose = ( args.Argc() > 1 && idStr::Icmp( args.Argv( 1 ), "v" ) == 0 );
	static_cast<void>( activeContext->shaders.CompileAll( verbose ) );
}

void idD3D12ImageViewer::Reset() {
	name.Clear();
	texture = textureHandle_t::NONE;
	reported = false;
}

// A texture recreated under the same name (a reload, vid_restart, a resize) is found again.
void idD3D12ImageViewer::Frame( idD3D12Context & context ) {
	const char * wanted = r_d3d12ShowImage.GetString();
	if ( wanted[0] == '\0' || idStr::Cmp( wanted, "0" ) == 0 ) {
		if ( !name.IsEmpty() ) {
			Reset();
		}
		return;
	}
	if ( name.Icmp( wanted ) != 0 ) {
		name = wanted;
		texture = textureHandle_t::NONE;
		reported = false;
	}
	if ( context.textures.Find( texture ) == nullptr ) {
		texture = context.textures.FindByName( name.c_str() );
		if ( texture == textureHandle_t::NONE ) {
			if ( !reported ) {
				common->Printf( "r_d3d12ShowImage: no texture named '%s'\n", name.c_str() );
				reported = true;
			}
			return;
		}
	}
	context.ShowImage( texture );
}

void idD3D12StatsReport::Reset() {
	sum = {};
	maxUploadBytes = 0;
	maxGpuMicroseconds = 0;
	frames = 0;
	overlay[0] = '\0';
}

// Called on the main thread while the game thread is idle, so the overlay line never changes under DrawFPS.
void idD3D12StatsReport::Frame( const idD3D12Context & context ) {
	if ( !r_d3d12Stats.GetBool() ) {
		if ( frames > 0 || overlay[0] != '\0' ) {
			Reset();
		}
		return;
	}
	const int now = Sys_Milliseconds();
	if ( frames == 0 ) {
		startMsec = now;
	}
	const d3d12FrameStats_t & last = context.LastFrameStats();
	sum.draw.draws += last.draw.draws;
	sum.draw.skipped += last.draw.skipped;
	sum.draw.psoBinds += last.draw.psoBinds;
	sum.draw.tableCopies += last.draw.tableCopies;
	sum.draw.unboundSlots += last.draw.unboundSlots;
	sum.draw.incompleteBinds += last.draw.incompleteBinds;
	sum.draw.chunkSubmits += last.draw.chunkSubmits;
	sum.draw.reserveWaits += last.draw.reserveWaits;
	sum.draw.parmBytes += last.draw.parmBytes;
	sum.psoCreated += last.psoCreated;
	sum.nearestBiases += last.nearestBiases;
	sum.debugGroups += last.debugGroups;
	sum.timestamps += last.timestamps;
	sum.gpuMicroseconds += last.gpuMicroseconds;
	sum.submits += last.submits;
	sum.waits += last.waits;
	sum.barriers += last.barriers;
	sum.uploadBytes += last.uploadBytes;
	sum.inPlaceBytes += last.inPlaceBytes;
	sum.samplerTables += last.samplerTables;
	sum.colorCopies += last.colorCopies;
	sum.depthCopies += last.depthCopies;
	sum.copySurfaces += last.copySurfaces;
	maxUploadBytes = Max( maxUploadBytes, last.uploadBytes );
	maxGpuMicroseconds = Max( maxGpuMicroseconds, last.gpuMicroseconds );
	frames++;
	if ( now - startMsec < REPORT_MSEC ) {
		return;
	}

	const double perFrame = 1.0 / frames;
	const idD3D12BufferStore & buffers = context.buffers;
	const d3d12DrawStats_t & draw = sum.draw;
	common->Printf( "d3d12 stats: %d frames; per frame: %.1f draws (%.1f skipped, %.1f unbound, %.1f incomplete), %.1f PSO binds, %.1f PSOs created, "
		"%d depth biases (%.1f draws with the nearest one), "
		"%.1f SRV tables, %.1f KB parms, %.1f debug groups; uploads %.1f KB staged (max %.1f KB) + %.1f KB in place, "
		"%.1f submits (%.1f after their draws), %.1f waits (%.1f in Reserve), "
		"%.1f barriers, %.1f sampler tables built; copies %.1f colour + %.1f depth, %d new surfaces; "
		"buffers %d: FRAME %.1f MB, STATIC %.1f MB + %.1f MB mirror; textures %d: %.1f MB; "
		"staging %.1f of %.0f MB, constants %.1f of %.0f MB; GPU %.2f ms (max %.2f ms), %.1f timestamps\n",
		frames, draw.draws * perFrame, draw.skipped * perFrame, draw.unboundSlots * perFrame, draw.incompleteBinds * perFrame, draw.psoBinds * perFrame,
		sum.psoCreated * perFrame, context.pipelines.Bias().Num(), sum.nearestBiases * perFrame, draw.tableCopies * perFrame,
		Kilobytes( draw.parmBytes ) * perFrame, sum.debugGroups * perFrame,
		Kilobytes( sum.uploadBytes ) * perFrame, Kilobytes( maxUploadBytes ), Kilobytes( sum.inPlaceBytes ) * perFrame,
		sum.submits * perFrame, draw.chunkSubmits * perFrame, sum.waits * perFrame, draw.reserveWaits * perFrame, sum.barriers * perFrame,
		sum.samplerTables * perFrame, sum.colorCopies * perFrame, sum.depthCopies * perFrame, sum.copySurfaces, buffers.Num(),
		Megabytes( buffers.Bytes( bufferUsage_t::FRAME ) ), Megabytes( buffers.Bytes( bufferUsage_t::STATIC ) ), Megabytes( buffers.MirrorBytes() ),
		context.textures.Num(), Megabytes( context.textures.Bytes() ), Megabytes( context.staging.Used() ), Megabytes( context.staging.Size() ),
		Megabytes( context.constants.Used() ), Megabytes( context.constants.Size() ),
		Milliseconds( sum.gpuMicroseconds ) * perFrame, Milliseconds( maxGpuMicroseconds ), sum.timestamps * perFrame );
	idStr::snPrintf( overlay, sizeof( overlay ), "draws %.0f  pso %.0f  F %.1f  S %.1f  up %.2f MB", draw.draws * perFrame, draw.psoBinds * perFrame,
		Megabytes( buffers.Bytes( bufferUsage_t::FRAME ) ), Megabytes( buffers.Bytes( bufferUsage_t::STATIC ) ),
		Megabytes( sum.uploadBytes + sum.inPlaceBytes ) * perFrame );
	sum = {};
	maxUploadBytes = 0;
	maxGpuMicroseconds = 0;
	frames = 0;
}
