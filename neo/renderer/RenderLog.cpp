/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company. 

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
#include "tr_local.h"

#include <array>
#include <vector>

/*
================================================================================================
Contains the RenderLog implementation.

TODO:	Emit statistics to the logfile at the end of views and frames.
================================================================================================
*/

idCVar r_logLevel( "r_logLevel", "2", CVAR_INTEGER, "1 = blocks only, 2 = everything", 1, 2 );

static const int LOG_LEVEL_BLOCKS_ONLY	= 1;
static const int LOG_LEVEL_EVERYTHING	= 2;

/*
================================================================================================

GPU / CPU time per render pass and debug groups (r_pix)

The backend marks its passes with renderLog.OpenMainBlock( MRB_* ) / CloseMainBlock(), and the
smaller pieces in them (views, lights, materials) with renderLog.OpenBlock( label ) / CloseBlock().

r_pix 1 times every main block with two GPU timestamps (renderBackend->WriteTimestamp), so blocks
can nest. The timestamps of a frame go into one of QUERY_FRAMES sets, and a set is read back only
once the GPU has written its last timestamp, a frame or more later, so the CPU never waits for the
GPU. About once a second the GPU and CPU milliseconds of each pass, per frame, are printed.

r_pix 2 wraps every main block and block in a debug group (a PIX event), so PIX, RenderDoc, Radeon
GPU Profiler and Nsight show named passes, views, lights and materials.

r_pix 4 has the backend time each of its own passes (renderBackend->TimePasses): the render graph's,
NRD's dispatches among them, printed by name about once a second. The bits add up.

Only the backend opens blocks, and the backend runs on the main thread with com_smp 0 and 1, the
thread that records the command lists. Events from any other thread are dropped. The results are read
back in SwapCommandBuffers, while the game thread is idle, so the com_showFPS overlay can read them
while the game thread builds the next frame.
================================================================================================
*/

idCVar r_pix( "r_pix", "0", CVAR_RENDERER | CVAR_INTEGER,
	"GPU time per render pass, bits: 1 = print the GPU and CPU ms of each pass about once a second "
	"(GPU timestamps, also adds the top passes to com_showFPS), 2 = debug groups named after "
	"the passes, views, lights and materials for PIX / RenderDoc / Radeon GPU Profiler / Nsight, "
	"4 = print the GPU ms of every render graph pass (NRD's by denoiser) about once a second",
	0, 7 );

/*
========================
RenderPassName

The name in the r_pix table and of the debug group.
========================
*/
static constexpr const char * RenderPassName( const renderLogMainBlock_t pass ) {
	switch ( pass ) {
		case MRB_NONE:						return "None";
		case MRB_BEGIN_DRAWING_VIEW:		return "Begin drawing view";
		case MRB_FILL_DEPTH_BUFFER:			return "Fill depth buffer";
		case MRB_GBUFFER:					return "G-buffer";
		case MRB_DRAW_INTERACTIONS:			return "Interactions";
		case MRB_STENCIL_SHADOWS:			return "Stencil shadows";
		case MRB_LIGHT_INTERACTIONS:		return "Light interactions";
		case MRB_TRANSLUCENT_INTERACTIONS:	return "Translucent interactions";
		case MRB_RT_SHADOWS:				return "Traced shadows";
		case MRB_DRAW_SHADER_PASSES:		return "Shader passes";
		case MRB_DRAW_GUI:					return "GUI";
		case MRB_FOG_ALL_LIGHTS:			return "Fog and blend lights";
		case MRB_DRAW_SHADER_PASSES_POST:	return "Shader passes post";
		case MRB_DRAW_DEBUG_TOOLS:			return "Debug tools";
		case MRB_CAPTURE_COLORBUFFER:		return "Framebuffer copies";
		case MRB_POSTPROCESS:				return "Post process";
		case MRB_HDR:						return "HDR resolve";
		case MRB_RT_SCENE:					return "Ray tracing scene";
		case MRB_RT_REFLECTIONS:			return "Reflections";
		case MRB_RT_INDIRECT:				return "Indirect light";
		case MRB_RT_DEBUG:					return "Ray tracing debug";
		case MRB_GPU_SYNC:					return "GPU sync";
		case MRB_END_FRAME:					return "End frame";
		case MRB_BINK_FRAME:				return "Bink frame";
		case MRB_BINK_NEXT_FRAME:			return "Bink next frame";
		case MRB_TOTAL:						return "Backend frame";
		case MRB_MAX:						break;
	}
	return "?";
}

/*
================================================
idRenderPassTimer
================================================
*/
class idRenderPassTimer {
public:
	void	BeginFrame();
	void	EndFrame();
	void	BeginMainBlock( renderLogMainBlock_t pass );
	void	EndMainBlock();
	void	BeginNamedEvent( const char * name );
	void	EndNamedEvent();
	void	FinishRendering( uint64 gpuTotalMicroseconds );
	int		GetPassTimings( std::span<renderPassTiming_t> passes ) const;
	void	Shutdown();

private:
	static constexpr int	TIMING			= BIT( 0 );	// r_pix 1
	static constexpr int	DEBUG_GROUPS	= BIT( 1 );	// r_pix 2
	static constexpr int	BACKEND_PASSES	= BIT( 2 );	// r_pix 4

	static constexpr int	QUERY_FRAMES	= TIMESTAMP_SETS;	// frames of timestamps in flight
	static constexpr int	MAX_BLOCKS		= 1024;		// timed main blocks in a frame, the frame itself included
	static constexpr int	MAX_DEPTH		= 32;		// nesting of the timed main blocks
	static constexpr int	MAX_GROUP_DEPTH	= 48;		// nesting of the debug groups
	static constexpr int	REPORT_MSEC		= 1000;

	struct block_t {
		renderLogMainBlock_t	pass;
		int						depth;			// 0 = the whole frame, 1 = a top level pass
		uint64					cpuStart;		// Sys_Microseconds() when it was opened
		uint64					cpuMicroSec;
	};

	// the timestamps of blocks[i] are slots 2 * i and 2 * i + 1 of the backend's set of this frame
	struct frame_t {
		std::vector<block_t>	blocks;			// blocks[0] is the whole frame
		int						numUntimed = 0;	// main blocks over MAX_BLOCKS or MAX_DEPTH
		bool					pending = false;// the timestamps are issued and not read back yet
	};

	// ms per frame, over the frames of a report period
	struct passStats_t {
		double	gpuSum = 0.0;
		double	gpuMax = 0.0;
		double	cpuSum = 0.0;
		double	cpuMax = 0.0;
		int		count = 0;				// instances of the pass
		int		depth = MAX_DEPTH;		// the least nesting it was seen at

		void	Add( double gpuMs, double cpuMs, int instances, int atDepth );
	};

	bool	IsBackendThread() const { return Sys_GetCurrentThreadID() == backendThread; }
	void	AbortFrame();
	void	OpenTimedBlock( renderLogMainBlock_t pass );
	void	CloseTimedBlock();
	void	PushGroup( const char * name );
	void	PopGroup();
	void	FetchResults();
	void	ReadFrame( int set );
	void	PublishPeriod();
	void	PrintPeriod( int periodMsec ) const;
	void	ResetPeriod();
	void	FreeQueries();

	// the frame being drawn, backend thread only
	int								frameFlags = 0;			// r_pix bits latched for the frame, 0 between frames
	int								requestedFlags = 0;		// r_pix when the missing extensions were reported
	uintptr_t						backendThread = 0;
	std::array<frame_t, QUERY_FRAMES>	frames;
	int								currentFrame = 0;
	std::array<int, MAX_DEPTH>		openBlocks = {};		// indexes into the blocks of the frame, -1 = not timed
	int								blockDepth = 0;
	int								groupDepth = 0;
	int								maxGroupDepth = 0;		// 0 until the caps are read
	bool							timestampsWritten = false;	// the backend holds timestamp slots of ours
	std::vector<uint64>				timestamps;				// scratch of ReadFrame

	// the report period, main thread only
	std::array<passStats_t, MRB_MAX>	periodPasses;
	passStats_t						periodOther;			// the time of the frame outside all top level passes
	double							timeElapsedSum = 0.0;	// the GL_TIME_ELAPSED total that com_showFPS shows
	double							timeElapsedMax = 0.0;
	int								timeElapsedFrames = 0;
	int								periodFrames = 0;		// frames read back
	int								periodUntimedBlocks = 0;
	int								periodSkippedFrames = 0;
	int								periodStartMsec = 0;
	bool							periodStarted = false;

	// published at the end of a period for com_showFPS, written while the game thread is idle
	std::array<renderPassTiming_t, MRB_MAX>	published = {};
	int								numPublished = 0;
};

static idRenderPassTimer renderPassTimer;

/*
========================
idRenderPassTimer::passStats_t::Add
========================
*/
void idRenderPassTimer::passStats_t::Add( const double gpuMs, const double cpuMs, const int instances, const int atDepth ) {
	gpuSum += gpuMs;
	gpuMax = Max( gpuMax, gpuMs );
	cpuSum += cpuMs;
	cpuMax = Max( cpuMax, cpuMs );
	count += instances;
	depth = Min( depth, atDepth );
}

/*
========================
idRenderPassTimer::BeginFrame

Called by the backend before it draws anything of the frame.
========================
*/
void idRenderPassTimer::BeginFrame() {
	if ( frameFlags != 0 ) {
		// the backend returned without EndFrame, for an empty command list
		AbortFrame();
	}

	int flags = r_pix.GetInteger();
	renderBackend->TimePasses( ( flags & BACKEND_PASSES ) != 0 && renderBackend->Caps().timestamps );
	flags &= TIMING | DEBUG_GROUPS;
	if ( flags != requestedFlags ) {
		requestedFlags = flags;
		if ( ( flags & TIMING ) != 0 && !renderBackend->Caps().timestamps ) {
			common->Printf( "r_pix: no GPU time per pass, %s has no timestamps here\n", renderBackend->Caps().apiName );
		}
		if ( ( flags & DEBUG_GROUPS ) != 0 && !renderBackend->Caps().debugGroups ) {
			common->Printf( "r_pix: no debug groups, %s has none here\n", renderBackend->Caps().apiName );
		}
	}
	if ( flags == 0 ) {
		return;
	}
	if ( !renderBackend->Caps().timestamps ) {
		flags &= ~TIMING;
	}
	if ( !renderBackend->Caps().debugGroups ) {
		flags &= ~DEBUG_GROUPS;
	}

	if ( ( flags & TIMING ) != 0 ) {
		FetchResults();
		if ( frames[currentFrame].pending ) {
			// the GPU is QUERY_FRAMES frames behind, skip timing this frame rather than wait for it
			periodSkippedFrames++;
			flags &= ~TIMING;
		}
	}
	if ( flags == 0 ) {
		return;
	}

	if ( ( flags & DEBUG_GROUPS ) != 0 && maxGroupDepth == 0 ) {
		maxGroupDepth = idMath::ClampInt( 1, MAX_GROUP_DEPTH, renderBackend->Caps().maxDebugGroupDepth );
	}

	backendThread = Sys_GetCurrentThreadID();
	frameFlags = flags;
	blockDepth = 0;
	groupDepth = 0;

	if ( ( flags & TIMING ) != 0 ) {
		frame_t & frame = frames[currentFrame];
		frame.blocks.clear();
		frame.numUntimed = 0;
		OpenTimedBlock( MRB_TOTAL );
	}
}

/*
========================
idRenderPassTimer::EndFrame

Called by the backend after the last command of the frame.
========================
*/
void idRenderPassTimer::EndFrame() {
	if ( frameFlags == 0 || !IsBackendThread() ) {
		return;
	}
	if ( ( frameFlags & TIMING ) != 0 ) {
		// close what unbalanced code left open, then the frame itself
		while ( blockDepth > 0 ) {
			CloseTimedBlock();
		}
		frames[currentFrame].pending = true;
		currentFrame = ( currentFrame + 1 ) % QUERY_FRAMES;
	}
	while ( groupDepth > 0 ) {
		PopGroup();
	}
	frameFlags = 0;
}

/*
========================
idRenderPassTimer::AbortFrame

Drops the frame being drawn, its timestamps are never read.
========================
*/
void idRenderPassTimer::AbortFrame() {
	while ( groupDepth > 0 ) {
		PopGroup();
	}
	blockDepth = 0;
	frameFlags = 0;
}

/*
========================
idRenderPassTimer::BeginMainBlock
========================
*/
void idRenderPassTimer::BeginMainBlock( const renderLogMainBlock_t pass ) {
	if ( frameFlags == 0 || !IsBackendThread() ) {
		return;
	}
	if ( ( frameFlags & DEBUG_GROUPS ) != 0 ) {
		PushGroup( RenderPassName( pass ) );
	}
	if ( ( frameFlags & TIMING ) != 0 ) {
		OpenTimedBlock( pass );
	}
}

/*
========================
idRenderPassTimer::EndMainBlock
========================
*/
void idRenderPassTimer::EndMainBlock() {
	if ( frameFlags == 0 || !IsBackendThread() ) {
		return;
	}
	// the frame itself, at depth 0, is closed by EndFrame
	if ( ( frameFlags & TIMING ) != 0 && blockDepth > 1 ) {
		CloseTimedBlock();
	}
	if ( ( frameFlags & DEBUG_GROUPS ) != 0 ) {
		PopGroup();
	}
}

/*
========================
idRenderPassTimer::BeginNamedEvent
========================
*/
void idRenderPassTimer::BeginNamedEvent( const char * name ) {
	if ( ( frameFlags & DEBUG_GROUPS ) == 0 || !IsBackendThread() ) {
		return;
	}
	PushGroup( name );
}

/*
========================
idRenderPassTimer::EndNamedEvent
========================
*/
void idRenderPassTimer::EndNamedEvent() {
	if ( ( frameFlags & DEBUG_GROUPS ) == 0 || !IsBackendThread() ) {
		return;
	}
	PopGroup();
}

/*
========================
idRenderPassTimer::OpenTimedBlock
========================
*/
void idRenderPassTimer::OpenTimedBlock( const renderLogMainBlock_t pass ) {
	frame_t & frame = frames[currentFrame];

	int index = -1;
	if ( blockDepth < MAX_DEPTH && frame.blocks.size() < MAX_BLOCKS ) {
		index = static_cast<int>( frame.blocks.size() );
		renderBackend->WriteTimestamp( currentFrame, index * 2 );
		timestampsWritten = true;
		frame.blocks.push_back( { pass, blockDepth, Sys_Microseconds(), 0 } );
	} else {
		frame.numUntimed++;
	}

	if ( blockDepth < MAX_DEPTH ) {
		openBlocks[blockDepth] = index;
	}
	blockDepth++;
}

/*
========================
idRenderPassTimer::CloseTimedBlock
========================
*/
void idRenderPassTimer::CloseTimedBlock() {
	if ( blockDepth <= 0 ) {
		return;
	}
	blockDepth--;
	const int index = ( blockDepth < MAX_DEPTH ) ? openBlocks[blockDepth] : -1;
	if ( index < 0 ) {
		return;
	}
	frame_t & frame = frames[currentFrame];
	block_t & block = frame.blocks[index];
	block.cpuMicroSec = Sys_Microseconds() - block.cpuStart;
	renderBackend->WriteTimestamp( currentFrame, index * 2 + 1 );
}

/*
========================
idRenderPassTimer::PushGroup

Past the depth the backend allows the group is only counted, so the pops stay balanced.
========================
*/
void idRenderPassTimer::PushGroup( const char * name ) {
	if ( groupDepth < maxGroupDepth ) {
		renderBackend->PushDebugGroup( ( name != nullptr && name[0] != '\0' ) ? name : "?" );
	}
	groupDepth++;
}

/*
========================
idRenderPassTimer::PopGroup

A pop without a push, as an unbalanced CloseBlock() would do, is dropped.
========================
*/
void idRenderPassTimer::PopGroup() {
	if ( groupDepth <= 0 ) {
		return;
	}
	groupDepth--;
	if ( groupDepth < maxGroupDepth ) {
		renderBackend->PopDebugGroup();
	}
}

/*
========================
idRenderPassTimer::FetchResults

Reads back the query sets the GPU has finished, oldest first. Never waits for the GPU.
========================
*/
void idRenderPassTimer::FetchResults() {
	for ( int i = 0; i < QUERY_FRAMES; i++ ) {
		const int set = ( currentFrame + i ) % QUERY_FRAMES;
		frame_t & frame = frames[set];
		if ( !frame.pending ) {
			continue;
		}
		// the end of the whole frame is the last timestamp of the set
		if ( !renderBackend->TimestampReady( set, 1 ) ) {
			break;	// the newer sets aren't done either
		}
		ReadFrame( set );
		frame.pending = false;
	}
}

/*
========================
idRenderPassTimer::ReadFrame
========================
*/
void idRenderPassTimer::ReadFrame( const int set ) {
	const frame_t & frame = frames[set];
	timestamps.resize( frame.blocks.size() * 2 );
	renderBackend->ReadTimestamps( set, static_cast<int>( timestamps.size() ), timestamps.data() );

	std::array<double, MRB_MAX> gpuMs = {};
	std::array<double, MRB_MAX> cpuMs = {};
	std::array<int, MRB_MAX> count = {};
	std::array<int, MRB_MAX> depth;
	depth.fill( MAX_DEPTH );
	double topLevelGpuMs = 0.0;
	double topLevelCpuMs = 0.0;

	for ( size_t i = 0; i < frame.blocks.size(); i++ ) {
		const block_t & block = frame.blocks[i];
		const uint64 begin = timestamps[i * 2];
		const uint64 end = timestamps[i * 2 + 1];
		const double blockGpuMs = ( end > begin ) ? ( end - begin ) * 1.0e-6 : 0.0;
		const double blockCpuMs = block.cpuMicroSec * 1.0e-3;

		gpuMs[block.pass] += blockGpuMs;
		cpuMs[block.pass] += blockCpuMs;
		count[block.pass]++;
		depth[block.pass] = Min( depth[block.pass], block.depth );
		if ( block.depth == 1 ) {
			topLevelGpuMs += blockGpuMs;
			topLevelCpuMs += blockCpuMs;
		}
	}

	for ( int pass = 0; pass < MRB_MAX; pass++ ) {
		if ( count[pass] > 0 ) {
			periodPasses[pass].Add( gpuMs[pass], cpuMs[pass], count[pass], depth[pass] );
		}
	}
	periodOther.Add( Max( 0.0, gpuMs[MRB_TOTAL] - topLevelGpuMs ), Max( 0.0, cpuMs[MRB_TOTAL] - topLevelCpuMs ), 1, 1 );
	periodUntimedBlocks += frame.numUntimed;
	periodFrames++;
}

/*
========================
idRenderPassTimer::FinishRendering

Called after the swap of the previous frame, while the game thread is idle.
========================
*/
void idRenderPassTimer::FinishRendering( const uint64 gpuTotalMicroseconds ) {
	if ( ( r_pix.GetInteger() & TIMING ) == 0 || !renderBackend->Caps().timestamps ) {
		if ( periodStarted || timestampsWritten ) {
			// r_pix 1 was turned off: give the queries back
			FreeQueries();
			ResetPeriod();
			numPublished = 0;
			periodStarted = false;
		}
		return;
	}

	FetchResults();

	const double timeElapsedMs = gpuTotalMicroseconds * 1.0e-3;
	timeElapsedSum += timeElapsedMs;
	timeElapsedMax = Max( timeElapsedMax, timeElapsedMs );
	timeElapsedFrames++;

	const int now = Sys_Milliseconds();
	if ( !periodStarted ) {
		periodStarted = true;
		periodStartMsec = now;
		return;
	}
	if ( now - periodStartMsec < REPORT_MSEC ) {
		return;
	}
	PublishPeriod();
	PrintPeriod( now - periodStartMsec );
	ResetPeriod();
	periodStartMsec = now;
}

/*
========================
idRenderPassTimer::PublishPeriod

Keeps the top level passes of the period for com_showFPS, the most expensive first.
========================
*/
void idRenderPassTimer::PublishPeriod() {
	numPublished = 0;
	if ( periodFrames == 0 ) {
		return;
	}
	for ( int pass = 0; pass < MRB_MAX; pass++ ) {
		const passStats_t & stats = periodPasses[pass];
		if ( pass == MRB_TOTAL || stats.count == 0 || stats.depth != 1 ) {
			continue;
		}
		published[numPublished++] = { RenderPassName( static_cast<renderLogMainBlock_t>( pass ) ), static_cast<float>( stats.gpuSum / periodFrames ) };
	}
	std::sort( published.begin(), published.begin() + numPublished, []( const renderPassTiming_t & a, const renderPassTiming_t & b ) {
		return a.gpuMilliseconds > b.gpuMilliseconds;
	} );
}

/*
========================
idRenderPassTimer::PrintPeriod
========================
*/
void idRenderPassTimer::PrintPeriod( const int periodMsec ) const {
	if ( periodFrames == 0 ) {
		// quiet when the backend drew nothing, as in a level load
		if ( periodSkippedFrames > 0 ) {
			common->Printf( "r_pix: no timestamps came back from the GPU in %.2f s, %d frames not timed\n", periodMsec * 0.001f, periodSkippedFrames );
		}
		return;
	}

	const double frameScale = 1.0 / periodFrames;
	const auto printRow = [frameScale]( const char * name, const char * count, const passStats_t & stats ) {
		common->Printf( "%-28s %6s %8.3f %8.3f %8.3f %8.3f\n", name, count,
			stats.gpuSum * frameScale, stats.gpuMax, stats.cpuSum * frameScale, stats.cpuMax );
	};

	common->Printf( "----- r_pix: ms per frame, average and max of %d frames in %.2f s -----\n", periodFrames, periodMsec * 0.001f );
	common->Printf( "%-28s %6s %8s %8s %8s %8s\n", "pass", "count", "GPU avg", "GPU max", "CPU avg", "CPU max" );
	for ( int pass = 0; pass < MRB_MAX; pass++ ) {
		const passStats_t & stats = periodPasses[pass];
		if ( pass == MRB_TOTAL || stats.count == 0 ) {
			continue;
		}
		// nested passes are indented under the pass they are drawn in
		const int indent = idMath::ClampInt( 0, 8, ( stats.depth - 1 ) * 2 );
		char name[64];
		idStr::snPrintf( name, sizeof( name ), "%*s%s", indent, "", RenderPassName( static_cast<renderLogMainBlock_t>( pass ) ) );
		char count[16];
		idStr::snPrintf( count, sizeof( count ), "%.1f", stats.count * frameScale );
		printRow( name, count, stats );
	}
	printRow( "Outside the passes", "", periodOther );
	printRow( RenderPassName( MRB_TOTAL ), "", periodPasses[MRB_TOTAL] );
	if ( timeElapsedFrames > 0 && renderBackend->Caps().gpuTimer ) {
		// the GL_TIME_ELAPSED query of com_showFPS and com_speeds, it should match the backend frame
		common->Printf( "%-28s %6s %8.3f %8.3f\n", "GPU total (time elapsed)", "", timeElapsedSum / timeElapsedFrames, timeElapsedMax );
	}
	if ( periodSkippedFrames > 0 ) {
		common->Printf( "%d frames not timed, the GPU was %d frames behind\n", periodSkippedFrames, QUERY_FRAMES );
	}
	if ( periodUntimedBlocks > 0 ) {
		common->Printf( "%d passes not timed, over %d per frame or nested over %d deep\n", periodUntimedBlocks, MAX_BLOCKS, MAX_DEPTH );
	}
}

/*
========================
idRenderPassTimer::ResetPeriod
========================
*/
void idRenderPassTimer::ResetPeriod() {
	periodPasses.fill( {} );
	periodOther = {};
	timeElapsedSum = 0.0;
	timeElapsedMax = 0.0;
	timeElapsedFrames = 0;
	periodFrames = 0;
	periodUntimedBlocks = 0;
	periodSkippedFrames = 0;
}

/*
========================
idRenderPassTimer::FreeQueries
========================
*/
void idRenderPassTimer::FreeQueries() {
	if ( ( frameFlags & TIMING ) != 0 ) {
		AbortFrame();
	}
	if ( timestampsWritten && renderBackend != nullptr ) {
		renderBackend->FreeTimestamps();
	}
	timestampsWritten = false;
	for ( frame_t & frame : frames ) {
		frame = {};
	}
	currentFrame = 0;
}

/*
========================
idRenderPassTimer::GetPassTimings
========================
*/
int idRenderPassTimer::GetPassTimings( std::span<renderPassTiming_t> passes ) const {
	const int num = Min( numPublished, static_cast<int>( passes.size() ) );
	std::copy_n( published.begin(), num, passes.begin() );
	return num;
}

/*
========================
idRenderPassTimer::Shutdown
========================
*/
void idRenderPassTimer::Shutdown() {
	AbortFrame();
	FreeQueries();
	ResetPeriod();
	numPublished = 0;
	periodStarted = false;
	maxGroupDepth = 0;
	requestedFlags = 0;
}

/*
================================================================================================

PC_* entry points of the render log, see RenderLog.h

================================================================================================
*/

void PC_BeginFrame() {
	renderPassTimer.BeginFrame();
}

void PC_EndFrame() {
	renderPassTimer.EndFrame();
}

void PC_BeginMainBlock( const renderLogMainBlock_t block ) {
	renderPassTimer.BeginMainBlock( block );
}

void PC_EndMainBlock() {
	renderPassTimer.EndMainBlock();
}

void PC_BeginNamedEvent( const char * name ) {
	renderPassTimer.BeginNamedEvent( name );
}

void PC_EndNamedEvent() {
	renderPassTimer.EndNamedEvent();
}

void PC_FinishRendering( const uint64 gpuTotalMicroseconds ) {
	renderPassTimer.FinishRendering( gpuTotalMicroseconds );
}

int PC_GetPassTimings( std::span<renderPassTiming_t> passes ) {
	return renderPassTimer.GetPassTimings( passes );
}

void PC_Shutdown() {
	renderPassTimer.Shutdown();
}


/*
================================================================================================

idRenderLog

================================================================================================
*/

idRenderLog	renderLog;

#if !defined( STUB_RENDER_LOG )

/*
========================
idRenderLog::idRenderLog
========================
*/
idRenderLog::idRenderLog() {
	activeLevel = 0;
	indentString[0] = '\0';
	indentLevel = 0;
	logFile = NULL;

	frameStartTime = 0;
	closeBlockTime = 0;
	logLevel = 0;
}

/*
========================
idRenderLog::StartFrame
========================
*/
void idRenderLog::StartFrame() {
	// r_pix works without the log file
	PC_BeginFrame();

	if ( r_logFile.GetInteger() == 0 ) {
		return;
	}

	// open a new logfile
	indentLevel = 0;
	indentString[0] = '\0';
	activeLevel = r_logLevel.GetInteger();

	struct tm		*newtime;
	time_t			aclock;

	char ospath[ MAX_OSPATH ];

	char qpath[128];
	sprintf( qpath, "renderlogPC_%04i.txt", r_logFile.GetInteger() );
	idStr finalPath = fileSystem->RelativePathToOSPath( qpath );		
	sprintf( ospath, "%s", finalPath.c_str() );
	/*
	for ( int i = 0; i < 9999 ; i++ ) {
		char qpath[128];
		sprintf( qpath, "renderlog_%04i.txt", r_logFile.GetInteger() );
		idStr finalPath = fileSystem->RelativePathToOSPath( qpath );
		fileSystem->RelativePathToOSPath( qpath, ospath, MAX_OSPATH ,FSPATH_BASE );
		if ( !fileSystem->FileExists( finalPath.c_str() ) ) {
			break; // use this name
		}
	}
	*/

	common->SetRefreshOnPrint( false );	// problems are caused if this print causes a refresh...

	if ( logFile != NULL ) {
		fileSystem->CloseFile( logFile );
		logFile = NULL;
	}

	logFile = fileSystem->OpenFileWrite( ospath );	
	if ( logFile == NULL ) {
		idLib::Warning( "Failed to open logfile %s", ospath );
		return;
	}
	idLib::Printf( "Opened logfile %s\n", ospath );

	// write the time out to the top of the file
	time( &aclock );
	newtime = localtime( &aclock );
	const char *str = asctime( newtime );
	logFile->Printf( "// %s", str );
	logFile->Printf( "// %s\n\n", com_version.GetString() );

	frameStartTime = Sys_Microseconds();
	closeBlockTime = frameStartTime;
	OpenBlock( "Frame" );
}

/*
========================
idRenderLog::EndFrame
========================
*/
void idRenderLog::EndFrame() {
	if ( logFile != NULL ) {
		if ( r_logFile.GetInteger() == 1 ) {
			Close();
		}
		// log is open, so decrement r_logFile and stop if it is zero
		r_logFile.SetInteger( r_logFile.GetInteger() - 1 );
		idLib::Printf( "Frame logged.\n" );
	}

	// after Close(), so the "Frame" block of the log closes inside the frame
	PC_EndFrame();
}

/*
========================
idRenderLog::Close
========================
*/
void idRenderLog::Close() {
	if ( logFile != NULL ) {
		CloseBlock();
		idLib::Printf( "Closing logfile\n" );
		fileSystem->CloseFile( logFile );
		logFile = NULL;
		activeLevel = 0;
	}
}

/*
========================
idRenderLog::OpenMainBlock
========================
*/
void idRenderLog::OpenMainBlock( renderLogMainBlock_t block ) {
	PC_BeginMainBlock( block );
}

/*
========================
idRenderLog::CloseMainBlock
========================
*/
void idRenderLog::CloseMainBlock() {
	PC_EndMainBlock();
}

/*
========================
idRenderLog::OpenBlock
========================
*/
void idRenderLog::OpenBlock( const char *label ) {
	// Allow the PIX functionality even when logFile is not running.
	PC_BeginNamedEvent( label );

	if ( logFile != NULL ) {
		LogOpenBlock( RENDER_LOG_INDENT_MAIN_BLOCK, label, NULL );
	}
}

/*
========================
idRenderLog::CloseBlock
========================
*/
void idRenderLog::CloseBlock() {
	PC_EndNamedEvent();

	if ( logFile != NULL ) {
		LogCloseBlock( RENDER_LOG_INDENT_MAIN_BLOCK );
	}
}

/*
========================
idRenderLog::Printf
========================
*/
void idRenderLog::Printf( const char *fmt, ... ) {
	if ( activeLevel <= LOG_LEVEL_BLOCKS_ONLY ) {
		return;
	}

	if ( logFile == NULL ) {
		return;
	}

	va_list marker;
	logFile->Printf( "%s", indentString );
	va_start( marker, fmt );
	logFile->VPrintf( fmt, marker );
	va_end( marker );
//	logFile->Flush();		this makes it take waaaay too long
}

/*
========================
idRenderLog::LogOpenBlock
========================
*/
void idRenderLog::LogOpenBlock( renderLogIndentLabel_t label, const char * fmt, va_list args ) {

	uint64 now = Sys_Microseconds();

	if ( logFile != NULL ) {
		if ( now - closeBlockTime >= 1000 ) {
			logFile->Printf( "%s%1.1f msec gap from last closeblock\n", indentString, ( now - closeBlockTime ) * ( 1.0f / 1000.0f ) );
		}
		logFile->Printf( "%s", indentString );
		logFile->VPrintf( fmt, args );
		logFile->Printf( " {\n" );
	}

	Indent( label );

	if ( logLevel >= MAX_LOG_LEVELS ) {
		idLib::Warning( "logLevel %d >= MAX_LOG_LEVELS", logLevel );
	}


	logLevel++;
}

/*
========================
idRenderLog::LogCloseBlock
========================
*/
void idRenderLog::LogCloseBlock( renderLogIndentLabel_t label ) {
	closeBlockTime = Sys_Microseconds();

	assert( logLevel > 0 );
	logLevel--;

	Outdent( label );

	if ( logFile != NULL ) {
	}
}

#endif // !STUB_RENDER_LOG
