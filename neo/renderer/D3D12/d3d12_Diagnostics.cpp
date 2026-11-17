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
	"1 = print the buffer memory and the uploads, submits, waits and barriers per frame of Direct3D 12 once a second, and show them under com_showFPS" );

namespace {

constexpr int REPORT_MSEC = 1000;

[[nodiscard]] constexpr double Megabytes( uint64 bytes ) {
	return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 );
}

[[nodiscard]] constexpr double Kilobytes( uint64 bytes ) {
	return static_cast<double>( bytes ) / 1024.0;
}

}	// namespace

void idD3D12StatsReport::Reset() {
	sum = {};
	maxUploadBytes = 0;
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
	sum.submits += last.submits;
	sum.waits += last.waits;
	sum.barriers += last.barriers;
	sum.uploadBytes += last.uploadBytes;
	sum.inPlaceBytes += last.inPlaceBytes;
	maxUploadBytes = Max( maxUploadBytes, last.uploadBytes );
	frames++;
	if ( now - startMsec < REPORT_MSEC ) {
		return;
	}

	const double perFrame = 1.0 / frames;
	const idD3D12BufferStore & buffers = context.buffers;
	common->Printf( "d3d12 stats: %d frames; per frame: uploads %.1f KB staged (max %.1f KB) + %.1f KB in place, %.1f submits, %.1f waits, "
		"%.1f barriers; buffers %d: FRAME %.1f MB, STATIC %.1f MB + %.1f MB mirror; staging %.1f of %.0f MB, constants %.1f of %.0f MB\n",
		frames, Kilobytes( sum.uploadBytes ) * perFrame, Kilobytes( maxUploadBytes ), Kilobytes( sum.inPlaceBytes ) * perFrame,
		sum.submits * perFrame, sum.waits * perFrame, sum.barriers * perFrame, buffers.Num(), Megabytes( buffers.Bytes( bufferUsage_t::FRAME ) ),
		Megabytes( buffers.Bytes( bufferUsage_t::STATIC ) ), Megabytes( buffers.MirrorBytes() ), Megabytes( context.staging.Used() ),
		Megabytes( context.staging.Size() ), Megabytes( context.constants.Used() ), Megabytes( context.constants.Size() ) );
	idStr::snPrintf( overlay, sizeof( overlay ), "F %.1f  S %.1f  up %.2f MB", Megabytes( buffers.Bytes( bufferUsage_t::FRAME ) ),
		Megabytes( buffers.Bytes( bufferUsage_t::STATIC ) ), Megabytes( sum.uploadBytes + sum.inPlaceBytes ) * perFrame );
	sum = {};
	maxUploadBytes = 0;
	frames = 0;
}
