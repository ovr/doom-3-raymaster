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

#include "../idlib/precompiled.h"
#pragma hdrstop

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "VisualDemo.h"
#include "../renderer/RenderBackend.h"

/*
===============================================================================

	Visual demos

	vdemo <name>... [quit]		runs the named demos in this order
	vdemo all [quit]			runs every vdemos/*.vdemo in name order, except the include-only
								ones whose names start with '_'
	vdemo stop					aborts the run

	A demo is a text timeline in base/vdemos/<name>.vdemo, one directive per line; a line that
	starts with '#' or '//' is a comment. Tic 0 is the first game tic after the map load.

	map <map>					the map to load, as for the map command (game/mars_city1)
	set <cvar> <value>			set before the map loads, restored when the demo ends
	at <tic> <command>			one console command, run just before game tic <tic>
	move <from> <to> <keys>		scripted input for tics from..to-1; overlapping moves add up
								forward=<-127..127>  right=<-127..127>  up=<1|-1> (jump, crouch)
								yaw=<deg/s> (+ turns left)  pitch=<deg/s> (+ looks down)
								attack  run  zoom  use  jump  crouch
	impulse <tic> <n>			an impulse with tic <tic>: 0..12 select weapons, 16 the flashlight
	capture <from> <to> [every=<k>] [fps=<n>]
								write the frames of tics from..to (inclusive), of every k-th tic
								from <from>, and of the sub-tic steps at n frames per second
								(default: every step of the virtual clock)
	end <tic>					the last tic; the next demo starts after it
	include <name>				reads vdemos/<name>.vdemo at this point (a second pass of a demo
								with other cvars, or shared setup)

	Frames are written to fs_savepath/base/vdemos/<demo>/t<tic, 5 digits>_s<step>.png, with a
	vdemo.json per demo (frameTags: the tic, step and render alpha of every frame) and
	vdemos/run.json for the run (and the render API it ran on). <tic> is the last game tic run
	before the frame was built. vdemo_timeoutScale stretches the load and stall timeouts for slow
	runs (GPU-based validation).

	Every demo starts from the same setup (fixedSetup below): r_swapInterval 0, rs_enable 0,
	g_showHud 0 (the view weapon stays), g_fov 80, g_skill 1, con_noPrint 1, com_wipeSeconds 0,
	timescale 1. The launcher (tools/vdemo/run-vdemos.ps1) adds what needs a restart: a 1280x720
	window, s_noSound 1, com_skipStartupVideos 1, in_mouse 0 and an isolated fs_savepath.

	Virtual clock: from the vdemo command to the end of the run, every read of the frame loop's
	game clock (frameTimeUS in idCommonLocal::Frame) is one step of 1 / vdemo_fps (240) seconds,
	aligned on the grid of the tic it is in, so a tic is due every vdemo_fps / com_engineHz (4)
	steps whatever the speed of the machine, and com_maxFPS is not applied. The coupled loop
	(com_decoupledTics 0) waits for each tic and renders step 0 only; the decoupled loop renders
	every step, steps 0..3 at render alpha 0, .25, .5, .75. The scripted view angles are a
	function of the virtual time, so frames between tics can turn smoothly.

	Capture: the game thread builds a frame into render system frame F, and with com_smp 1 the
	back end renders it during the next Frame(), so the tag is keyed by F and the back buffer is
	read right after the back end rendered F, before its swap (as TakeScreenshot reads its own
	render). Worker threads encode the PNGs (zlib takes ~40 ms a frame), the main thread writes
	them in capture order and waits for them only at the end of a demo or with 16 frames queued.

	Determinism: single player seeds gameLocal.random with 0 on every map load (LoadMap). Sound
	is off (s_noSound 1, needs a restart): muted sound would still be mixed by the wall clock,
	and sound-driven materials and script waits on sound lengths would follow it. The Flash GUIs
	(HUD, PDA) animate by idSWF::Milliseconds, which reads GuiMilliseconds: the virtual clock
	counted from the demo's start, so HUD frames and SWF random seeds repeat too.

===============================================================================
*/

idCVar vdemo_fps( "vdemo_fps", "240", CVAR_SYSTEM | CVAR_INTEGER, "steps per second of the visual demo clock, a multiple of com_engineHz" );
idCVar vdemo_capture( "vdemo_capture", "1", CVAR_SYSTEM | CVAR_BOOL, "0 runs the visual demos without writing their frames" );
idCVar vdemo_realtime( "vdemo_realtime", "0", CVAR_SYSTEM | CVAR_BOOL, "1 plays the visual demos at the speed of their clock, for watching; 0 runs them as fast as possible. The frames are the same" );
idCVar vdemo_timeoutScale( "vdemo_timeoutScale", "1", CVAR_SYSTEM | CVAR_FLOAT, "multiplies the map load and stall timeouts of the visual demos, for slow runs such as GPU-based validation", 1.0f, 100.0f );

extern idCVar s_noSound;

namespace {

constexpr int		MAX_INCLUDE_DEPTH = 4;
// with com_smp 1 the last tagged frame reaches the back end one Frame() later
constexpr int		DRAIN_FRAMES = 3;
constexpr int		MAX_LOAD_ATTEMPTS = 2;
// a load that hangs, or fails without an ERP_DROP error
constexpr int		LOAD_TIMEOUT_MSEC = 90000;
constexpr int		STALL_TIMEOUT_MSEC = 60000;
constexpr int		EXPECTED_WIDTH = 1280;
constexpr int		EXPECTED_HEIGHT = 720;

[[nodiscard]] int ScaledTimeoutMsec( int msec ) {
	return static_cast<int>( msec * vdemo_timeoutScale.GetFloat() );
}

struct cvarSetting_t {
	idStr		name;
	idStr		value;
};

// the same for every demo, so one demo's settings never leak into the next
constexpr struct {
	const char *	name;
	const char *	value;
} fixedSetup[] = {
	{ "r_swapInterval",				"0" },
	{ "rs_enable",					"0" },
	{ "com_showFPS",				"0" },
	{ "com_speeds",					"0" },
	{ "con_noPrint",				"1" },
	{ "com_wipeSeconds",			"0" },
	{ "timescale",					"1" },
	{ "com_fixedTic",				"0" },
	{ "com_noSleep",				"0" },
	{ "g_stopTime",					"0" },
	{ "g_showHud",					"0" },
	{ "ui_showGun",					"1" },
	{ "g_fov",						"80" },
	{ "g_skill",					"1" },
	{ "pm_thirdPerson",				"0" },
	{ "g_skipViewEffects",			"0" },
	{ "g_showcamerainfo",			"0" },
	{ "r_useStencilShadowPreload",	"1" },
	{ "com_disableAutoSaves",		"1" },
};

struct timedCommand_t {
	int			tic;
	idStr		text;
};

struct move_t {
	int			from;
	int			to;
	int			forward;
	int			right;
	int			buttons;
	double		yawSpeed;		// degrees per second
	double		pitchSpeed;
};

struct impulse_t {
	int			tic;
	int			impulse;
};

struct capture_t {
	int			from;
	int			to;
	int			every;
	int			fps;
};

struct demo_t {
	idStr							name;
	idStr							map;
	int								endTic = -1;
	std::vector<cvarSetting_t>		cvars;
	std::vector<timedCommand_t>		commands;
	std::vector<move_t>				moves;
	std::vector<impulse_t>			impulses;
	std::vector<capture_t>			captures;
};

struct frameTag_t {
	int			frameCount;		// the render system frame the game thread builds it into
	idStr		demo;
	int			tic;
	int			step;
	float		alpha;
};

struct result_t {
	idStr		name;
	idStr		map;
	idStr		status;
	std::vector<frameTag_t>	written;
	int			frames = 0;
	int			tics = 0;
	int			loadAttempts = 0;
	int			loadMsec = 0;
	int			runMsec = 0;
};

/*
========================
SplitWords

Whitespace separated words; a word in double quotes may hold spaces.
========================
*/
std::vector<idStr> SplitWords( const char * text ) {
	std::vector<idStr> words;
	const char * s = text;
	while ( *s != '\0' ) {
		while ( *s == ' ' || *s == '\t' ) {
			s++;
		}
		if ( *s == '\0' ) {
			break;
		}
		idStr word;
		if ( *s == '"' ) {
			s++;
			while ( *s != '\0' && *s != '"' ) {
				word.Append( *s++ );
			}
			if ( *s == '"' ) {
				s++;
			}
		} else {
			while ( *s != '\0' && *s != ' ' && *s != '\t' ) {
				word.Append( *s++ );
			}
		}
		words.push_back( word );
	}
	return words;
}

/*
========================
SkipWords

The text after the first count words, so that a console command keeps its own quoting.
========================
*/
const char * SkipWords( const char * text, int count ) {
	const char * s = text;
	for ( int i = 0; i < count; i++ ) {
		while ( *s == ' ' || *s == '\t' ) {
			s++;
		}
		while ( *s != '\0' && *s != ' ' && *s != '\t' ) {
			s++;
		}
	}
	while ( *s == ' ' || *s == '\t' ) {
		s++;
	}
	return s;
}

bool ParseInt( const char * text, int & out ) {
	char * end = nullptr;
	const long value = std::strtol( text, &end, 10 );
	if ( end == text || *end != '\0' ) {
		return false;
	}
	out = static_cast<int>( value );
	return true;
}

bool ParseDouble( const char * text, double & out ) {
	char * end = nullptr;
	const double value = std::strtod( text, &end );
	if ( end == text || *end != '\0' ) {
		return false;
	}
	out = value;
	return true;
}

short AngleToShort( double degrees ) {
	const int units = static_cast<int>( std::floor( degrees * ( 65536.0 / 360.0 ) + 0.5 ) );
	return static_cast<short>( static_cast<uint16>( units & 0xFFFF ) );
}

int ButtonForName( const char * name ) {
	constexpr struct {
		const char *	name;
		int				button;
	} buttons[] = {
		{ "attack",	BUTTON_ATTACK },
		{ "run",	BUTTON_RUN },
		{ "zoom",	BUTTON_ZOOM },
		{ "use",	BUTTON_USE },
		{ "jump",	BUTTON_JUMP },
		{ "crouch",	BUTTON_CROUCH },
	};
	for ( const auto & b : buttons ) {
		if ( idStr::Icmp( name, b.name ) == 0 ) {
			return b.button;
		}
	}
	return 0;
}

/*
========================
EncodePNG

8-bit RGB, every row Paeth filtered. bottomUp rows come from glReadPixels.
========================
*/
int Paeth( int a, int b, int c ) {
	const int p = a + b - c;
	const int pa = abs( p - a );
	const int pb = abs( p - b );
	const int pc = abs( p - c );
	if ( pa <= pb && pa <= pc ) {
		return a;
	}
	return ( pb <= pc ) ? b : c;
}

void AppendBigEndian( std::vector<byte> & out, uint32 value ) {
	out.push_back( static_cast<byte>( value >> 24 ) );
	out.push_back( static_cast<byte>( value >> 16 ) );
	out.push_back( static_cast<byte>( value >> 8 ) );
	out.push_back( static_cast<byte>( value ) );
}

void AppendChunk( std::vector<byte> & out, const char * type, const byte * data, size_t size ) {
	AppendBigEndian( out, static_cast<uint32>( size ) );
	const size_t typeStart = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data, data + size );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc = crc32( crc, &out[typeStart], static_cast<uInt>( size + 4 ) );
	AppendBigEndian( out, static_cast<uint32>( crc ) );
}

bool EncodePNG( const byte * rgb, int width, int height, bool bottomUp, std::vector<byte> & out ) {
	const size_t stride = static_cast<size_t>( width ) * 3;
	std::vector<byte> filtered( ( stride + 1 ) * height );
	const byte * previous = nullptr;
	for ( int y = 0; y < height; y++ ) {
		const byte * row = rgb + stride * ( bottomUp ? height - 1 - y : y );
		byte * dest = &filtered[( stride + 1 ) * y];
		*dest++ = 4;
		for ( size_t x = 0; x < stride; x++ ) {
			const int a = ( x >= 3 ) ? row[x - 3] : 0;
			const int b = ( previous != nullptr ) ? previous[x] : 0;
			const int c = ( previous != nullptr && x >= 3 ) ? previous[x - 3] : 0;
			dest[x] = static_cast<byte>( row[x] - Paeth( a, b, c ) );
		}
		previous = row;
	}

	uLongf compressedSize = compressBound( static_cast<uLong>( filtered.size() ) );
	std::vector<byte> compressed( compressedSize );
	// level 2: 4x faster than the default level 6 (~40 against ~170 ms a frame), files 10% bigger
	if ( compress2( compressed.data(), &compressedSize, filtered.data(), static_cast<uLong>( filtered.size() ), 2 ) != Z_OK ) {
		return false;
	}

	static const byte signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector<byte> header;
	AppendBigEndian( header, static_cast<uint32>( width ) );
	AppendBigEndian( header, static_cast<uint32>( height ) );
	header.insert( header.end(), { 8, 2, 0, 0, 0 } );	// 8 bits, RGB, deflate, standard filters, no interlace

	out.clear();
	out.insert( out.end(), signature, signature + 8 );
	AppendChunk( out, "IHDR", header.data(), header.size() );
	AppendChunk( out, "IDAT", compressed.data(), compressedSize );
	AppendChunk( out, "IEND", nullptr, 0 );
	return true;
}

/*
========================
idFrameEncoder

Encodes captured frames to PNG on worker threads. A job belongs to the main thread, which frees it
only after IsDone, so a worker never touches a freed job.
========================
*/
struct encodeJob_t {
	frameTag_t			tag;
	int					width = 0;
	int					height = 0;
	std::vector<byte>	pixels;		// bottom-up RGB rows of glReadPixels
	std::vector<byte>	png;
	bool				encoded = false;
	bool				done = false;	// guarded by the encoder's mutex
};

class idFrameEncoder {
public:
	void Start() {
		if ( !workers.empty() ) {
			return;
		}
		const int count = idMath::ClampInt( 1, 6, static_cast<int>( std::thread::hardware_concurrency() ) - 2 );
		for ( int i = 0; i < count; i++ ) {
			workers.emplace_back( [this]( std::stop_token stop ) { Work( stop ); } );
		}
	}

	// finishes the queued jobs, then joins the workers
	void Stop() {
		for ( std::jthread & worker : workers ) {
			worker.request_stop();
		}
		workers.clear();
	}

	void Submit( encodeJob_t * job ) {
		{
			std::scoped_lock lock( mutex );
			jobs.push_back( job );
		}
		wake.notify_one();
	}

	bool IsDone( const encodeJob_t * job ) {
		std::scoped_lock lock( mutex );
		return job->done;
	}

	void WaitDone( const encodeJob_t * job ) {
		std::unique_lock lock( mutex );
		finished.wait( lock, [job] { return job->done; } );
	}

private:
	void Work( std::stop_token stop ) {
		while ( true ) {
			encodeJob_t * job = nullptr;
			{
				std::unique_lock lock( mutex );
				if ( !wake.wait( lock, stop, [this] { return !jobs.empty(); } ) ) {
					return;
				}
				job = jobs.front();
				jobs.pop_front();
			}
			job->encoded = EncodePNG( job->pixels.data(), job->width, job->height, true, job->png );
			job->pixels = std::vector<byte>();
			{
				std::scoped_lock lock( mutex );
				job->done = true;
			}
			finished.notify_all();
		}
	}

	std::mutex						mutex;
	std::condition_variable_any		wake;
	std::condition_variable			finished;
	std::deque<encodeJob_t *>		jobs;
	std::vector<std::jthread>		workers;
};

/*
===============================================================================

	idVisualDemoLocal

===============================================================================
*/
class idVisualDemoLocal final : public idVisualDemo {
public:
	bool			IsRunning() const override { return state != state_t::IDLE; }
	int				MaxFrameRate() const override { return vdemo_realtime.GetBool() ? vdemo_fps.GetInteger() : 0; }
	uint64			FrameLoopMicroseconds( int ticMilliseconds, double residual ) override;
	int				GuiMilliseconds() const override;
	void			RunFrame( bool gameRunning, int numGameFrames, float renderAlpha, usercmd_t & cmd ) override;
	void			OnMapSpawned() override { mapSpawned = true; }
	void			OnDropError() override { dropped = true; }
	void			CaptureRenderedFrame() override;

	void			Command( const idCmdArgs & args );

private:
	enum class state_t {
		IDLE,
		NEXT_DEMO,
		LOADING,
		RUNNING,
		DRAINING
	};

	static std::vector<idStr>	ListDemos();
	bool			LoadDemo( const char * name, demo_t & out ) const;
	bool			ParseFile( const char * name, demo_t & out, int depth ) const;
	bool			ParseLine( const char * file, int lineNum, const char * line, demo_t & out, int depth ) const;

	void			StartRun( std::vector<idStr> names, bool quit );
	void			FinishRun();
	void			BeginDemo();
	void			EndDemo( const char * status );

	void			SetCVar( const char * name, const char * value );
	void			RestoreCVars();

	void			RunCommands( int tic ) const;
	void			BuildUsercmd( int tic, int step, usercmd_t & cmd ) const;
	bool			WantsCapture( int tic, int step ) const;
	void			WriteFrame( const frameTag_t & tag );
	void			WriteEncodedFrames( size_t maxPending );
	void			WriteDemoSummary( const result_t & result ) const;
	void			WriteRunSummary() const;

	state_t			state = state_t::IDLE;
	std::vector<idStr>			queue;
	size_t			nextDemo = 0;
	bool			quitWhenDone = false;
	demo_t			demo;
	int				stepsPerTic = 1;

	// the frame loop's clock; the offset keeps the wall clock continuous after a run
	bool			virtualClock = false;
	uint64			lastFrameLoopTimeUS = 0;
	int64			wallOffsetUS = 0;
	int				step = 0;			// virtual steps since the last tic
	uint64			demoStartUS = 0;	// the virtual clock when the demo began, the origin of the GUI clock

	bool			mapSpawned = false;
	bool			dropped = false;
	int				nextTic = 0;		// the demo tic the next game tic runs
	int				drainFrames = 0;
	int				stateStartTime = 0;
	int				lastTicTime = 0;
	int				runStartTime = 0;
	bool			warnedSize = false;

	std::vector<cvarSetting_t>	savedCVars;
	std::vector<frameTag_t>		pendingFrames;
	std::vector<result_t>		results;

	static constexpr size_t		MAX_PENDING_FRAMES = 16;	// ~45 MB of frames in flight
	std::deque<std::unique_ptr<encodeJob_t>>	encoding;	// capture order
	idFrameEncoder				encoder;	// after encoding: its workers are joined before the jobs are freed
};

idVisualDemoLocal	visualDemoLocal;

/*
========================
idVisualDemoLocal::FrameLoopMicroseconds
========================
*/
uint64 idVisualDemoLocal::FrameLoopMicroseconds( int ticMilliseconds, double residual ) {
	if ( !virtualClock ) {
		lastFrameLoopTimeUS = static_cast<uint64>( static_cast<int64>( Sys_Microseconds() ) + wallOffsetUS );
		return lastFrameLoopTimeUS;
	}
	// a residual off the grid (wall time from before the run) is moved onto the next step;
	// the steps of a 16 or 17 msec tic are whole microseconds, so the residual stays on the grid
	const double stepLength = static_cast<double>( ticMilliseconds ) / stepsPerTic;
	const int current = idMath::ClampInt( 0, stepsPerTic - 1, static_cast<int>( std::floor( residual / stepLength + 1e-6 ) ) );
	const double target = ( current + 1 == stepsPerTic ) ? static_cast<double>( ticMilliseconds ) : ( current + 1 ) * stepLength;
	step = ( current + 1 ) % stepsPerTic;
	lastFrameLoopTimeUS += static_cast<uint64>( std::llround( Max( target - residual, 0.0 ) * 1000.0 ) );
	return lastFrameLoopTimeUS;
}

// Only the main thread moves the clock, between the game thread's frames. The base is nonzero
// because idSWF reads a last render time of 0 as "never rendered".
int idVisualDemoLocal::GuiMilliseconds() const {
	constexpr int GUI_CLOCK_BASE_MSEC = 1000000;
	if ( !virtualClock ) {
		return Sys_Milliseconds();
	}
	return GUI_CLOCK_BASE_MSEC + static_cast<int>( ( lastFrameLoopTimeUS - demoStartUS ) / 1000 );
}

/*
========================
idVisualDemoLocal::RunFrame
========================
*/
void idVisualDemoLocal::RunFrame( bool gameRunning, int numGameFrames, float renderAlpha, usercmd_t & cmd ) {
	if ( state == state_t::IDLE ) {
		return;
	}
	const int now = Sys_Milliseconds();
	usercmd_t scripted;

	if ( state == state_t::NEXT_DEMO ) {
		BeginDemo();
	}

	if ( state == state_t::LOADING ) {
		if ( mapSpawned ) {
			mapSpawned = false;
			results.back().loadMsec = now - stateStartTime;
			common->Printf( "vdemo %s: %s loaded in %d msec\n", demo.name.c_str(), demo.map.c_str(), now - stateStartTime );
			state = state_t::RUNNING;
			stateStartTime = now;
			lastTicTime = now;
			nextTic = 0;
		} else if ( dropped && results.back().loadAttempts < MAX_LOAD_ATTEMPTS ) {
			// the heatHaze GLSL link fails on the first load of some maps, and only then
			dropped = false;
			results.back().loadAttempts++;
			common->Warning( "vdemo %s: loading %s failed, retrying", demo.name.c_str(), demo.map.c_str() );
			cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "map %s\n", demo.map.c_str() ) );
		} else if ( dropped ) {
			EndDemo( "load error" );
		} else if ( now - stateStartTime > ScaledTimeoutMsec( LOAD_TIMEOUT_MSEC ) ) {
			common->Warning( "vdemo %s: %s didn't load", demo.name.c_str(), demo.map.c_str() );
			EndDemo( "load timeout" );
		}
	}

	if ( state == state_t::RUNNING || state == state_t::DRAINING ) {
		if ( dropped ) {
			EndDemo( "error" );
		}
	}

	if ( state == state_t::RUNNING ) {
		if ( mapSpawned ) {
			common->Warning( "vdemo %s: another map was loaded during the demo", demo.name.c_str() );
			EndDemo( "map changed" );
		} else if ( !gameRunning || numGameFrames <= 0 ) {
			const int stallTimeout = ScaledTimeoutMsec( STALL_TIMEOUT_MSEC );
			if ( !gameRunning && now - lastTicTime > stallTimeout ) {
				common->Warning( "vdemo %s: no game tics for %d seconds (paused?)", demo.name.c_str(), stallTimeout / 1000 );
				EndDemo( "stalled" );
			} else if ( gameRunning && nextTic > 0 ) {
				// a frame between tics
				BuildUsercmd( nextTic - 1, step, scripted );
				if ( WantsCapture( nextTic - 1, step ) ) {
					pendingFrames.push_back( { renderSystem->GetFrameCount(), demo.name, nextTic - 1, step, renderAlpha } );
				}
			}
		} else if ( nextTic > demo.endTic ) {
			results.back().tics = nextTic;
			state = state_t::DRAINING;
			drainFrames = DRAIN_FRAMES;
		} else {
			for ( int i = 0; i < numGameFrames; i++ ) {
				RunCommands( nextTic + i );
			}
			nextTic += numGameFrames;
			lastTicTime = now;
			const int tic = nextTic - 1;
			BuildUsercmd( tic, step, scripted );
			if ( WantsCapture( tic, step ) ) {
				pendingFrames.push_back( { renderSystem->GetFrameCount(), demo.name, tic, step, renderAlpha } );
			}
		}
	} else if ( state == state_t::DRAINING ) {
		if ( --drainFrames <= 0 ) {
			EndDemo( "ok" );
		}
	}

	cmd = scripted;
}

/*
========================
idVisualDemoLocal::CaptureRenderedFrame
========================
*/
void idVisualDemoLocal::CaptureRenderedFrame() {
	WriteEncodedFrames( MAX_PENDING_FRAMES );
	if ( pendingFrames.empty() ) {
		return;
	}
	const int renderedFrame = renderSystem->GetFrameCount() - 1;
	for ( const frameTag_t & tag : pendingFrames ) {
		if ( tag.frameCount == renderedFrame ) {
			WriteFrame( tag );
		}
	}
	// a frame an out-of-loop UpdateScreen rendered is dropped
	std::erase_if( pendingFrames, [renderedFrame]( const frameTag_t & tag ) { return tag.frameCount <= renderedFrame; } );
}

/*
========================
idVisualDemoLocal::WriteFrame
========================
*/
void idVisualDemoLocal::WriteFrame( const frameTag_t & tag ) {
	if ( !vdemo_capture.GetBool() ) {
		return;
	}
	const int width = renderSystem->GetWidth();
	const int height = renderSystem->GetHeight();
	if ( ( width != EXPECTED_WIDTH || height != EXPECTED_HEIGHT ) && !warnedSize ) {
		common->Warning( "vdemo: the window is %dx%d, the baselines are %dx%d", width, height, EXPECTED_WIDTH, EXPECTED_HEIGHT );
		warnedSize = true;
	}

	WriteEncodedFrames( MAX_PENDING_FRAMES - 1 );

	auto job = std::make_unique<encodeJob_t>();
	job->tag = tag;
	job->width = width;
	job->height = height;
	job->pixels.resize( static_cast<size_t>( width ) * height * 3 );
	renderBackend->ReadOutputPixels( 0, 0, width, height, job->pixels.data() );

	encoder.Submit( job.get() );
	encoding.push_back( std::move( job ) );
}

/*
========================
idVisualDemoLocal::WriteEncodedFrames

Writes the encoded frames in capture order, waiting for the oldest while more than maxPending are left.
========================
*/
void idVisualDemoLocal::WriteEncodedFrames( const size_t maxPending ) {
	while ( !encoding.empty() ) {
		const encodeJob_t * job = encoding.front().get();
		if ( !encoder.IsDone( job ) ) {
			if ( encoding.size() <= maxPending ) {
				return;
			}
			encoder.WaitDone( job );
		}

		// not va(): the game thread runs during the capture and shares its ring of buffers, which
		// once wrote a frame to fs_savepath/base/0
		const frameTag_t & tag = job->tag;
		idStr fileName;
		fileName.Format( "vdemos/%s/t%05d_s%d.png", tag.demo.c_str(), tag.tic, tag.step );
		if ( !job->encoded ) {
			common->Warning( "vdemo %s: couldn't compress tic %d", tag.demo.c_str(), tag.tic );
		} else if ( fileSystem->WriteFile( fileName, job->png.data(), static_cast<int>( job->png.size() ) ) <= 0 ) {
			common->Warning( "vdemo: couldn't write %s", fileName.c_str() );
		} else {
			for ( auto it = results.rbegin(); it != results.rend(); ++it ) {
				if ( it->name == tag.demo ) {
					it->written.push_back( tag );
					it->frames++;
					break;
				}
			}
		}
		encoding.pop_front();
	}
}

/*
========================
idVisualDemoLocal::RunCommands
========================
*/
void idVisualDemoLocal::RunCommands( int tic ) const {
	for ( const timedCommand_t & command : demo.commands ) {
		if ( command.tic == tic ) {
			common->Printf( "vdemo %s: tic %d: %s\n", demo.name.c_str(), tic, command.text.c_str() );
			cmdSystem->BufferCommandText( CMD_EXEC_NOW, command.text );
		}
	}
}

/*
========================
idVisualDemoLocal::BuildUsercmd

The angles integrate the turn speeds up to the virtual time of the frame, tic + step / steps.
========================
*/
void idVisualDemoLocal::BuildUsercmd( int tic, int frameStep, usercmd_t & cmd ) const {
	const double time = tic + static_cast<double>( frameStep ) / stepsPerTic;
	const double engineHz = com_engineHz_latched;
	int forward = 0;
	int right = 0;
	int buttons = 0;
	double yaw = 0.0;
	double pitch = 0.0;
	for ( const move_t & move : demo.moves ) {
		if ( tic >= move.from && tic < move.to ) {
			forward += move.forward;
			right += move.right;
			buttons |= move.buttons;
		}
		const double seconds = Max( 0.0, Min( time, static_cast<double>( move.to ) ) - move.from ) / engineHz;
		yaw += move.yawSpeed * seconds;
		pitch += move.pitchSpeed * seconds;
	}

	int sequence = 0;
	int impulse = 0;
	for ( const impulse_t & i : demo.impulses ) {
		if ( i.tic <= tic ) {
			sequence++;
			impulse = i.impulse;
		}
	}

	cmd = usercmd_t();
	cmd.forwardmove = idMath::ClampChar( forward );
	cmd.rightmove = idMath::ClampChar( right );
	cmd.buttons = static_cast<byte>( buttons );
	cmd.angles[PITCH] = AngleToShort( pitch );
	cmd.angles[YAW] = AngleToShort( yaw );
	cmd.impulse = static_cast<byte>( impulse );
	cmd.impulseSequence = static_cast<byte>( sequence );
}

/*
========================
idVisualDemoLocal::WantsCapture
========================
*/
bool idVisualDemoLocal::WantsCapture( int tic, int frameStep ) const {
	const int clockFps = stepsPerTic * idMath::Ftoi( com_engineHz_latched );
	for ( const capture_t & capture : demo.captures ) {
		if ( tic < capture.from || tic > capture.to || ( tic - capture.from ) % capture.every != 0 ) {
			continue;
		}
		if ( frameStep % ( clockFps / capture.fps ) == 0 ) {
			return true;
		}
	}
	return false;
}

/*
========================
idVisualDemoLocal::SetCVar

Remembers the value from before the run the first time a cvar is set.
========================
*/
void idVisualDemoLocal::SetCVar( const char * name, const char * value ) {
	if ( cvarSystem->Find( name ) == nullptr ) {
		common->Warning( "vdemo: unknown cvar %s", name );
		return;
	}
	const bool saved = std::any_of( savedCVars.begin(), savedCVars.end(), [name]( const cvarSetting_t & s ) { return s.name.Icmp( name ) == 0; } );
	if ( !saved ) {
		savedCVars.push_back( { name, cvarSystem->GetCVarString( name ) } );
	}
	cvarSystem->SetCVarString( name, value );
}

void idVisualDemoLocal::RestoreCVars() {
	for ( auto it = savedCVars.rbegin(); it != savedCVars.rend(); ++it ) {
		cvarSystem->SetCVarString( it->name, it->value );
	}
	savedCVars.clear();
}

/*
========================
idVisualDemoLocal::ListDemos
========================
*/
std::vector<idStr> idVisualDemoLocal::ListDemos() {
	std::vector<idStr> names;
	idFileList * files = fileSystem->ListFiles( "vdemos", ".vdemo", true );
	for ( int i = 0; i < files->GetNumFiles(); i++ ) {
		idStr name = files->GetFile( i );
		name.StripFileExtension();
		if ( name.Length() > 0 && name[0] != '_' ) {
			names.push_back( name );
		}
	}
	fileSystem->FreeFileList( files );
	return names;
}

/*
========================
idVisualDemoLocal::ParseLine
========================
*/
bool idVisualDemoLocal::ParseLine( const char * file, int lineNum, const char * line, demo_t & out, int depth ) const {
	const std::vector<idStr> words = SplitWords( line );
	if ( words.empty() || words[0][0] == '#' || ( words[0][0] == '/' && words[0][1] == '/' ) ) {
		return true;
	}
	const auto fail = [&]( const char * message ) {
		common->Warning( "%s:%d: %s", file, lineNum, message );
		return false;
	};
	const char * op = words[0].c_str();
	const int numWords = static_cast<int>( words.size() );

	if ( idStr::Icmp( op, "map" ) == 0 ) {
		if ( numWords != 2 ) {
			return fail( "usage: map <map>" );
		}
		out.map = words[1];
	} else if ( idStr::Icmp( op, "set" ) == 0 ) {
		if ( numWords != 3 ) {
			return fail( "usage: set <cvar> <value>, quote a value with spaces" );
		}
		out.cvars.push_back( { words[1], words[2] } );
	} else if ( idStr::Icmp( op, "at" ) == 0 ) {
		timedCommand_t command;
		if ( numWords < 3 || !ParseInt( words[1], command.tic ) || command.tic < 0 ) {
			return fail( "usage: at <tic> <command>" );
		}
		command.text = SkipWords( line, 2 );
		out.commands.push_back( command );
	} else if ( idStr::Icmp( op, "move" ) == 0 ) {
		move_t move = {};
		if ( numWords < 3 || !ParseInt( words[1], move.from ) || !ParseInt( words[2], move.to ) || move.to < move.from ) {
			return fail( "usage: move <from> <to> forward=.. right=.. up=.. yaw=.. pitch=.. <buttons>" );
		}
		for ( int i = 3; i < numWords; i++ ) {
			const char * key = words[i].c_str();
			const char * equals = strchr( key, '=' );
			if ( equals == nullptr ) {
				const int button = ButtonForName( key );
				if ( button == 0 ) {
					return fail( va( "unknown move button '%s'", key ) );
				}
				move.buttons |= button;
				continue;
			}
			const idStr name( key, 0, static_cast<int>( equals - key ) );
			double value = 0.0;
			if ( !ParseDouble( equals + 1, value ) ) {
				return fail( va( "bad number in '%s'", key ) );
			}
			if ( name.Icmp( "forward" ) == 0 ) {
				move.forward = static_cast<int>( value );
			} else if ( name.Icmp( "right" ) == 0 ) {
				move.right = static_cast<int>( value );
			} else if ( name.Icmp( "up" ) == 0 ) {
				move.buttons |= ( value > 0.0 ) ? BUTTON_JUMP : ( value < 0.0 ) ? BUTTON_CROUCH : 0;
			} else if ( name.Icmp( "yaw" ) == 0 ) {
				move.yawSpeed = value;
			} else if ( name.Icmp( "pitch" ) == 0 ) {
				move.pitchSpeed = value;
			} else {
				return fail( va( "unknown move key '%s'", name.c_str() ) );
			}
		}
		out.moves.push_back( move );
	} else if ( idStr::Icmp( op, "impulse" ) == 0 ) {
		impulse_t impulse = {};
		if ( numWords != 3 || !ParseInt( words[1], impulse.tic ) || !ParseInt( words[2], impulse.impulse ) || impulse.impulse < 0 || impulse.impulse > 255 ) {
			return fail( "usage: impulse <tic> <n>" );
		}
		out.impulses.push_back( impulse );
	} else if ( idStr::Icmp( op, "capture" ) == 0 ) {
		capture_t capture = { 0, 0, 1, 0 };
		if ( numWords < 3 || !ParseInt( words[1], capture.from ) || !ParseInt( words[2], capture.to ) || capture.to < capture.from ) {
			return fail( "usage: capture <from> <to> [every=<k>] [fps=<n>]" );
		}
		for ( int i = 3; i < numWords; i++ ) {
			const char * key = words[i].c_str();
			bool ok = false;
			if ( idStr::Icmpn( key, "every=", 6 ) == 0 ) {
				ok = ParseInt( key + 6, capture.every ) && capture.every > 0;
			} else if ( idStr::Icmpn( key, "fps=", 4 ) == 0 ) {
				ok = ParseInt( key + 4, capture.fps ) && capture.fps > 0;
			}
			if ( !ok ) {
				return fail( va( "bad capture option '%s'", key ) );
			}
		}
		out.captures.push_back( capture );
	} else if ( idStr::Icmp( op, "end" ) == 0 ) {
		if ( numWords != 2 || !ParseInt( words[1], out.endTic ) || out.endTic < 0 ) {
			return fail( "usage: end <tic>" );
		}
	} else if ( idStr::Icmp( op, "include" ) == 0 ) {
		if ( numWords != 2 ) {
			return fail( "usage: include <name>" );
		}
		if ( depth >= MAX_INCLUDE_DEPTH ) {
			return fail( "includes nest too deep" );
		}
		return ParseFile( words[1], out, depth + 1 );
	} else {
		return fail( va( "unknown directive '%s'", op ) );
	}
	return true;
}

/*
========================
idVisualDemoLocal::ParseFile
========================
*/
bool idVisualDemoLocal::ParseFile( const char * name, demo_t & out, int depth ) const {
	const idStr path = va( "vdemos/%s.vdemo", name );
	void * buffer = nullptr;
	if ( fileSystem->ReadFile( path, &buffer ) < 0 || buffer == nullptr ) {
		common->Warning( "vdemo: can't read %s", path.c_str() );
		return false;
	}
	const idStr text( static_cast<const char *>( buffer ) );
	fileSystem->FreeFile( buffer );

	bool ok = true;
	int lineNum = 0;
	int lineStart = 0;
	while ( lineStart <= text.Length() ) {
		int lineEnd = text.Find( '\n', lineStart );
		if ( lineEnd < 0 ) {
			lineEnd = text.Length();
		}
		idStr line( text, lineStart, lineEnd );
		line.StripTrailingWhitespace();
		lineNum++;
		ok &= ParseLine( path, lineNum, line, out, depth );
		lineStart = lineEnd + 1;
	}
	return ok;
}

/*
========================
idVisualDemoLocal::LoadDemo
========================
*/
bool idVisualDemoLocal::LoadDemo( const char * name, demo_t & out ) const {
	out = demo_t();
	out.name = name;
	if ( !ParseFile( name, out, 0 ) ) {
		return false;
	}
	if ( out.map.IsEmpty() || out.endTic < 0 ) {
		common->Warning( "vdemo %s: needs a map and an end", name );
		return false;
	}
	const int clockFps = stepsPerTic * idMath::Ftoi( com_engineHz_latched );
	for ( capture_t & capture : out.captures ) {
		if ( capture.fps == 0 ) {
			capture.fps = clockFps;
		}
		if ( clockFps % capture.fps != 0 || capture.fps * stepsPerTic < clockFps ) {
			common->Warning( "vdemo %s: capture fps=%d must divide %d and be at least com_engineHz", name, capture.fps, clockFps );
			return false;
		}
	}
	// commands of the same tic run in the order of the file
	std::stable_sort( out.commands.begin(), out.commands.end(), []( const timedCommand_t & a, const timedCommand_t & b ) { return a.tic < b.tic; } );
	return true;
}

/*
========================
idVisualDemoLocal::StartRun
========================
*/
void idVisualDemoLocal::StartRun( std::vector<idStr> names, bool quit ) {
	const int engineHz = idMath::Ftoi( com_engineHz_latched );
	if ( engineHz <= 0 || vdemo_fps.GetInteger() % engineHz != 0 ) {
		common->Warning( "vdemo: vdemo_fps %d must be a multiple of com_engineHz %d", vdemo_fps.GetInteger(), engineHz );
		return;
	}
	if ( !s_noSound.GetBool() ) {
		common->Warning( "vdemo: s_noSound is 0, sound-driven materials won't repeat between runs (+set s_noSound 1 at launch)" );
	}
	stepsPerTic = vdemo_fps.GetInteger() / engineHz;
	encoder.Start();
	queue = std::move( names );
	nextDemo = 0;
	quitWhenDone = quit;
	results.clear();
	pendingFrames.clear();
	warnedSize = false;
	step = 0;
	virtualClock = true;
	demoStartUS = lastFrameLoopTimeUS;
	runStartTime = Sys_Milliseconds();
	state = state_t::NEXT_DEMO;
	common->Printf( "vdemo: %d demos, %d clock steps per tic\n", static_cast<int>( queue.size() ), stepsPerTic );
}

/*
========================
idVisualDemoLocal::FinishRun
========================
*/
void idVisualDemoLocal::FinishRun() {
	WriteEncodedFrames( 0 );
	encoder.Stop();
	RestoreCVars();
	state = state_t::IDLE;
	wallOffsetUS = static_cast<int64>( lastFrameLoopTimeUS ) - static_cast<int64>( Sys_Microseconds() );
	virtualClock = false;

	int frames = 0;
	int failed = 0;
	for ( const result_t & result : results ) {
		frames += result.frames;
		failed += ( result.status != "ok" ) ? 1 : 0;
	}
	WriteRunSummary();
	common->Printf( "vdemo: done, %d demos (%d failed), %d frames in %d sec\n", static_cast<int>( results.size() ), failed, frames, ( Sys_Milliseconds() - runStartTime ) / 1000 );
	if ( quitWhenDone ) {
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
	}
}

/*
========================
idVisualDemoLocal::BeginDemo
========================
*/
void idVisualDemoLocal::BeginDemo() {
	if ( nextDemo >= queue.size() ) {
		FinishRun();
		return;
	}
	const idStr name = queue[nextDemo];

	result_t result;
	result.name = name;
	if ( !LoadDemo( name, demo ) ) {
		result.status = "bad timeline";
		results.push_back( result );
		nextDemo++;
		return;
	}
	result.map = demo.map;
	result.status = "running";
	result.loadAttempts = 1;
	results.push_back( result );

	RestoreCVars();
	for ( const auto & setting : fixedSetup ) {
		SetCVar( setting.name, setting.value );
	}
	for ( const cvarSetting_t & setting : demo.cvars ) {
		SetCVar( setting.name, setting.value );
	}

	// frames of an earlier run with other capture windows would mix in
	idFileList * old = fileSystem->ListFiles( va( "vdemos/%s", name.c_str() ), ".png" );
	for ( int i = 0; i < old->GetNumFiles(); i++ ) {
		remove( fileSystem->RelativePathToOSPath( va( "vdemos/%s/%s", name.c_str(), old->GetFile( i ) ), "fs_savepath" ) );
	}
	fileSystem->FreeFileList( old );

	common->Printf( "vdemo %s: loading %s\n", name.c_str(), demo.map.c_str() );
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "map %s\n", demo.map.c_str() ) );
	mapSpawned = false;
	dropped = false;
	stateStartTime = Sys_Milliseconds();
	demoStartUS = lastFrameLoopTimeUS;
	state = state_t::LOADING;
}

/*
========================
idVisualDemoLocal::EndDemo
========================
*/
void idVisualDemoLocal::EndDemo( const char * status ) {
	WriteEncodedFrames( 0 );
	result_t & result = results.back();
	result.status = status;
	result.runMsec = Sys_Milliseconds() - stateStartTime;
	if ( result.tics == 0 ) {
		result.tics = nextTic;
	}
	WriteDemoSummary( result );
	common->Printf( "vdemo %s: %s, %d tics, %d frames in %d msec\n", result.name.c_str(), status, result.tics, result.frames, result.runMsec );
	dropped = false;
	nextDemo++;
	state = state_t::NEXT_DEMO;
}

/*
========================
idVisualDemoLocal::WriteDemoSummary
========================
*/
void idVisualDemoLocal::WriteDemoSummary( const result_t & result ) const {
	idStr json = va( "{\n\t\"demo\": \"%s\",\n\t\"map\": \"%s\",\n\t\"status\": \"%s\",\n\t\"clockFps\": %d,\n\t\"engineHz\": %d,\n\t\"stepsPerTic\": %d,\n"
		"\t\"endTic\": %d,\n\t\"tics\": %d,\n\t\"frames\": %d,\n\t\"width\": %d,\n\t\"height\": %d,\n\t\"smp\": %d,\n\t\"decoupledTics\": %d,\n"
		"\t\"loadAttempts\": %d,\n\t\"loadMsec\": %d,\n\t\"runMsec\": %d,\n\t\"frameTags\": [\n",
		result.name.c_str(), result.map.c_str(), result.status.c_str(), stepsPerTic * idMath::Ftoi( com_engineHz_latched ), idMath::Ftoi( com_engineHz_latched ), stepsPerTic,
		demo.endTic, result.tics, result.frames, renderSystem->GetWidth(), renderSystem->GetHeight(), cvarSystem->GetCVarInteger( "com_smp" ),
		cvarSystem->GetCVarInteger( "com_decoupledTics" ), result.loadAttempts, result.loadMsec, result.runMsec );
	for ( size_t i = 0; i < result.written.size(); i++ ) {
		const frameTag_t & tag = result.written[i];
		json += va( "\t\t{ \"file\": \"t%05d_s%d.png\", \"tic\": %d, \"step\": %d, \"alpha\": %g }%s\n",
			tag.tic, tag.step, tag.tic, tag.step, tag.alpha, ( i + 1 < result.written.size() ) ? "," : "" );
	}
	json += "\t]\n}\n";
	fileSystem->WriteFile( va( "vdemos/%s/vdemo.json", result.name.c_str() ), json.c_str(), json.Length() );
}

/*
========================
idVisualDemoLocal::WriteRunSummary
========================
*/
void idVisualDemoLocal::WriteRunSummary() const {
	idStr json = "{\n\t\"demos\": [\n";
	for ( size_t i = 0; i < results.size(); i++ ) {
		const result_t & r = results[i];
		json += va( "\t\t{ \"demo\": \"%s\", \"map\": \"%s\", \"status\": \"%s\", \"tics\": %d, \"frames\": %d, \"loadAttempts\": %d, \"loadMsec\": %d, \"runMsec\": %d }%s\n",
			r.name.c_str(), r.map.c_str(), r.status.c_str(), r.tics, r.frames, r.loadAttempts, r.loadMsec, r.runMsec, ( i + 1 < results.size() ) ? "," : "" );
	}
	const char * api = ( renderBackend != nullptr ) ? renderBackend->Caps().apiName : "none";
	json += va( "\t],\n\t\"api\": \"%s\",\n\t\"runMsec\": %d\n}\n", api, Sys_Milliseconds() - runStartTime );
	fileSystem->WriteFile( "vdemos/run.json", json.c_str(), json.Length() );
}

/*
========================
idVisualDemoLocal::Command
========================
*/
void idVisualDemoLocal::Command( const idCmdArgs & args ) {
	if ( args.Argc() >= 2 && idStr::Icmp( args.Argv( 1 ), "stop" ) == 0 ) {
		if ( IsRunning() ) {
			if ( state != state_t::NEXT_DEMO && !results.empty() ) {
				results.back().status = "stopped";
			}
			FinishRun();
		}
		return;
	}
	if ( IsRunning() ) {
		common->Printf( "vdemo: a run is in progress, 'vdemo stop' ends it\n" );
		return;
	}

	std::vector<idStr> names;
	bool quit = false;
	for ( int i = 1; i < args.Argc(); i++ ) {
		const char * arg = args.Argv( i );
		if ( idStr::Icmp( arg, "quit" ) == 0 ) {
			quit = true;
		} else if ( idStr::Icmp( arg, "all" ) == 0 ) {
			for ( const idStr & name : ListDemos() ) {
				names.push_back( name );
			}
		} else {
			names.push_back( arg );
		}
	}
	if ( names.empty() ) {
		common->Printf( "usage: vdemo <name>... [quit] | vdemo all [quit] | vdemo stop\n" );
		for ( const idStr & name : ListDemos() ) {
			common->Printf( "  %s\n", name.c_str() );
		}
		if ( quit ) {
			cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
		}
		return;
	}
	StartRun( std::move( names ), quit );
}

} // namespace

idVisualDemo * visualDemo = &visualDemoLocal;

CONSOLE_COMMAND_SHIP( vdemo, "runs visual demos from vdemos/*.vdemo: vdemo <name>... | all [quit], vdemo stop", nullptr ) {
	visualDemoLocal.Command( args );
}
