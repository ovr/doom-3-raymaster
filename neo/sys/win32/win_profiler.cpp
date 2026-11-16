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

===========================================================================
*/

/*
================================================================================================

	Sampling profiler

	A background thread periodically suspends the registered thread (the game thread),
	captures its call stack by unwinding it with the x64 unwind data (.pdata) of the
	loaded modules and resumes it. The report printed by profileReport (and at shutdown)
	lists the functions with the most samples, both self and inclusive, and the most
	common call paths.

	sys_profile 1		start sampling as soon as the game thread runs
	profileStart		start sampling
	profileStop			stop sampling and print the report
	profileReport		print the report of the samples collected so far

================================================================================================
*/

#include "../../idlib/precompiled.h"
#pragma hdrstop

#include <windows.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <psapi.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

idCVar sys_profile( "sys_profile", "0", CVAR_SYSTEM | CVAR_NOCHEAT | CVAR_BOOL, "sample the game thread call stacks from startup, the report is printed at shutdown (see profileReport)" );
idCVar sys_profileTop( "sys_profileTop", "40", CVAR_SYSTEM | CVAR_NOCHEAT | CVAR_INTEGER, "number of entries in each section of the profiler report" );

static const int	PROFILE_MAX_FRAMES		= 32;
static const int	PROFILE_MAX_SAMPLES		= 400000;	// ~6.5 minutes at 1 kHz

struct profileSample_t {
	int			numFrames;
	DWORD64		frames[PROFILE_MAX_FRAMES];
};

// the unwind tables of a loaded module, cached while the target thread runs
struct profileModule_t {
	DWORD64						base;
	DWORD64						end;
	const RUNTIME_FUNCTION *	functions;
	DWORD						numFunctions;
};

static const int			PROFILE_MAX_MODULES		= 512;
static const int			PROFILE_MODULES_REFRESH	= 1000;		// ms

static HANDLE				profileTargetThread = NULL;
static DWORD				profileTargetThreadId = 0;
static DWORD64				profileTargetStackBase = 0;
static profileModule_t		profileModules[PROFILE_MAX_MODULES];
static int					profileNumModules = 0;
static HANDLE				profileSamplerThread = NULL;
static volatile LONG		profileRunning = 0;
static profileSample_t *	profileSamples = NULL;
static volatile LONG		profileNumSamples = 0;
static volatile LONG		profileNumBusy = 0;
static int					profileStartTime = 0;
static int					profileStopTime = 0;

// Caches the .pdata of every loaded module. RtlLookupFunctionEntry can take the loader
// lock that the suspended thread may hold, so the sampler looks entries up itself.
static void Profile_RefreshModules() {
	HMODULE modules[PROFILE_MAX_MODULES];
	DWORD bytes = 0;
	if ( !EnumProcessModules( GetCurrentProcess(), modules, sizeof( modules ), &bytes ) ) {
		return;
	}
	const int count = Min( static_cast<int>( bytes / sizeof( HMODULE ) ), PROFILE_MAX_MODULES );
	int numModules = 0;
	for ( int i = 0; i < count; i++ ) {
		const auto base = reinterpret_cast<const BYTE *>( modules[i] );
		const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>( base );
		const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>( base + dos->e_lfanew );
		const IMAGE_DATA_DIRECTORY & pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
		if ( pdata.VirtualAddress == 0 || pdata.Size < sizeof( RUNTIME_FUNCTION ) ) {
			continue;
		}
		profileModule_t & module = profileModules[numModules++];
		module.base = reinterpret_cast<DWORD64>( base );
		module.end = module.base + nt->OptionalHeader.SizeOfImage;
		module.functions = reinterpret_cast<const RUNTIME_FUNCTION *>( base + pdata.VirtualAddress );
		module.numFunctions = pdata.Size / sizeof( RUNTIME_FUNCTION );
	}
	std::sort( profileModules, profileModules + numModules, []( const profileModule_t & a, const profileModule_t & b ) { return a.base < b.base; } );
	profileNumModules = numModules;
}

// binary search of the cached unwind tables, NULL for leaf functions and unknown code
static const RUNTIME_FUNCTION * Profile_LookupFunction( DWORD64 pc, DWORD64 & imageBase ) {
	const profileModule_t * modulesBegin = profileModules;
	const profileModule_t * modulesEnd = modulesBegin + profileNumModules;
	const profileModule_t * module = std::upper_bound( modulesBegin, modulesEnd, pc, []( DWORD64 value, const profileModule_t & m ) { return value < m.base; } );
	if ( module == modulesBegin ) {
		return NULL;
	}
	module--;
	if ( pc >= module->end ) {
		return NULL;
	}
	imageBase = module->base;
	const DWORD rva = static_cast<DWORD>( pc - module->base );
	const RUNTIME_FUNCTION * functionsEnd = module->functions + module->numFunctions;
	const RUNTIME_FUNCTION * function = std::upper_bound( module->functions, functionsEnd, rva, []( DWORD value, const RUNTIME_FUNCTION & f ) { return value < f.BeginAddress; } );
	if ( function == module->functions ) {
		return NULL;
	}
	function--;
	if ( rva >= function->EndAddress ) {
		return NULL;
	}
	// an indirect entry points at the primary entry of the function
	if ( function->UnwindData & 1 ) {
		function = reinterpret_cast<const RUNTIME_FUNCTION *>( module->base + function->UnwindData - 1 );
	}
	return function;
}

/*
========================
Profile_CaptureStack

Unwinds the suspended thread. Must not allocate or lock, the suspended thread may
hold the heap or the loader lock.
========================
*/
static int Profile_CaptureStack( const CONTEXT & threadContext, DWORD64 * frames ) {
	CONTEXT ctx = threadContext;
	int numFrames = 0;
	__try {
		while ( numFrames < PROFILE_MAX_FRAMES && ctx.Rip != 0 ) {
			frames[numFrames++] = ctx.Rip;

			const DWORD64 rsp = ctx.Rsp;
			DWORD64 imageBase = 0;
			const RUNTIME_FUNCTION * function = Profile_LookupFunction( ctx.Rip, imageBase );
			if ( function == NULL ) {
				// a leaf function doesn't touch the stack, its return address is on top
				ctx.Rip = *reinterpret_cast<const DWORD64 *>( ctx.Rsp );
				ctx.Rsp += 8;
			} else {
				void * handlerData = NULL;
				DWORD64 establisherFrame = 0;
				RtlVirtualUnwind( UNW_FLAG_NHANDLER, imageBase, ctx.Rip, const_cast<PRUNTIME_FUNCTION>( function ), &ctx, &handlerData, &establisherFrame, NULL );
			}
			if ( ctx.Rsp <= rsp || ctx.Rsp >= profileTargetStackBase ) {
				break;
			}
		}
	} __except ( EXCEPTION_EXECUTE_HANDLER ) {
	}
	return numFrames;
}

/*
========================
Profile_SamplerThread
========================
*/
static DWORD WINAPI Profile_SamplerThread( LPVOID ) {
	timeBeginPeriod( 1 );
	int modulesTime = 0;
	while ( profileRunning ) {
		Sleep( 1 );

		if ( profileTargetThread == NULL || profileNumSamples >= PROFILE_MAX_SAMPLES ) {
			continue;
		}
		const int now = Sys_Milliseconds();
		if ( now - modulesTime >= PROFILE_MODULES_REFRESH ) {
			Profile_RefreshModules();
			modulesTime = now;
		}
		if ( SuspendThread( profileTargetThread ) == (DWORD)-1 ) {
			continue;
		}

		CONTEXT ctx;
		memset( &ctx, 0, sizeof( ctx ) );
		ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		if ( GetThreadContext( profileTargetThread, &ctx ) ) {
			profileSample_t & sample = profileSamples[profileNumSamples];
			sample.numFrames = Profile_CaptureStack( ctx, sample.frames );
			profileNumSamples++;
		}

		ResumeThread( profileTargetThread );
	}
	timeEndPeriod( 1 );
	return 0;
}

/*
========================
Profile_Start
========================
*/
static void Profile_Start() {
	if ( profileRunning ) {
		return;
	}
	if ( profileSamples == NULL ) {
		profileSamples = (profileSample_t *)VirtualAlloc( NULL, sizeof( profileSample_t ) * PROFILE_MAX_SAMPLES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
		if ( profileSamples == NULL ) {
			idLib::Warning( "profiler: couldn't allocate the sample buffer" );
			return;
		}
	}
	profileNumSamples = 0;
	profileStartTime = Sys_Milliseconds();
	profileStopTime = 0;
	profileRunning = 1;
	profileSamplerThread = CreateThread( NULL, 0, Profile_SamplerThread, NULL, 0, NULL );
	if ( profileSamplerThread == NULL ) {
		profileRunning = 0;
		idLib::Warning( "profiler: couldn't create the sampler thread" );
		return;
	}
	SetThreadPriority( profileSamplerThread, THREAD_PRIORITY_TIME_CRITICAL );
	idLib::Printf( "profiler: sampling %s\n", profileTargetThread != NULL ? "the game thread" : "(waiting for the game thread)" );
}

/*
========================
Profile_Stop
========================
*/
static void Profile_Stop() {
	if ( !profileRunning ) {
		return;
	}
	profileRunning = 0;
	WaitForSingleObject( profileSamplerThread, INFINITE );
	CloseHandle( profileSamplerThread );
	profileSamplerThread = NULL;
	profileStopTime = Sys_Milliseconds();
}

/*
========================
Sys_ProfilerRegisterThread

Called by the thread that should be sampled.
========================
*/
void Sys_ProfilerRegisterThread() {
	const DWORD threadId = GetCurrentThreadId();
	if ( threadId == profileTargetThreadId ) {
		return;
	}
	if ( profileTargetThread != NULL ) {
		CloseHandle( profileTargetThread );
		profileTargetThread = NULL;
	}

	HANDLE thread = NULL;
	DuplicateHandle( GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
		THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0 );

	const NT_TIB * tib = (const NT_TIB *)NtCurrentTeb();
	profileTargetStackBase = reinterpret_cast<DWORD64>( tib->StackBase );
	profileTargetThreadId = threadId;
	profileTargetThread = thread;

	if ( sys_profile.GetBool() && !profileRunning && profileNumSamples == 0 ) {
		Profile_Start();
	}
}

/*
================================================================================================

	Report

================================================================================================
*/

class idProfileSymbols {
public:
	idProfileSymbols() {
		// a unique fake process handle, so this doesn't collide with other
		// users of dbghelp in the process (see Sym_Init in win_shared.cpp)
		process = (HANDLE)(UINT_PTR)0x0bf9d3b1;
		SymSetOptions( SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS );
		initialized = SymInitialize( process, NULL, FALSE ) != FALSE;
		if ( !initialized ) {
			idLib::Printf( "profiler: SymInitialize failed (error %u), no symbols\n", GetLastError() );
		}
		reportedFailure = false;
	}
	~idProfileSymbols() {
		if ( initialized ) {
			SymCleanup( process );
		}
	}

	// returns the function name for an address, "module!0xoffset" if there are no symbols
	const std::string & Name( DWORD64 addr ) {
		auto it = cache.find( addr );
		if ( it != cache.end() ) {
			return it->second;
		}

		std::string name;
		MEMORY_BASIC_INFORMATION mbi;
		if ( VirtualQuery( reinterpret_cast<void *>( addr ), &mbi, sizeof( mbi ) ) == 0 || mbi.AllocationBase == NULL ) {
			name = "???";
		} else {
			const DWORD64 base = reinterpret_cast<DWORD64>( mbi.AllocationBase );
			char modulePath[MAX_PATH];
			modulePath[0] = 0;
			GetModuleFileNameA( reinterpret_cast<HMODULE>( base ), modulePath, sizeof( modulePath ) );
			const char * moduleName = strrchr( modulePath, '\\' );
			moduleName = ( moduleName != NULL ) ? moduleName + 1 : modulePath;

			if ( initialized && loadedModules.find( base ) == loadedModules.end() ) {
				loadedModules.insert( base );
				SetLastError( 0 );
				if ( SymLoadModuleEx( process, NULL, modulePath, NULL, base, 0, NULL, 0 ) == 0 && GetLastError() != 0 ) {
					idLib::Printf( "profiler: SymLoadModuleEx( %s ) failed (error %u)\n", modulePath, GetLastError() );
				}
			}

			char symbolBuffer[sizeof( SYMBOL_INFO ) + 512];
			SYMBOL_INFO * symbol = (SYMBOL_INFO *)symbolBuffer;
			memset( symbol, 0, sizeof( SYMBOL_INFO ) );
			symbol->SizeOfStruct = sizeof( SYMBOL_INFO );
			symbol->MaxNameLen = 511;
			DWORD64 displacement = 0;
			if ( initialized && SymFromAddr( process, addr, &displacement, symbol ) ) {
				if ( idStr::Icmp( moduleName, "Doom3BFG.exe" ) == 0 ) {
					name = symbol->Name;
				} else {
					name = std::string( moduleName ) + "!" + symbol->Name;
				}
			} else {
				const DWORD error = GetLastError();
				if ( initialized && !reportedFailure && idStr::Icmp( moduleName, "Doom3BFG.exe" ) == 0 ) {
					reportedFailure = true;
					idLib::Printf( "profiler: SymFromAddr( 0x%llx ) failed (error %u)\n", addr, error );
				}
				char buffer[MAX_PATH + 32];
				idStr::snPrintf( buffer, sizeof( buffer ), "%s!0x%llx", moduleName, addr - base );
				name = buffer;
			}
		}
		return cache.emplace( addr, name ).first->second;
	}

private:
	HANDLE									process;
	bool									initialized;
	bool									reportedFailure;
	std::unordered_map< DWORD64, std::string >	cache;
	std::unordered_set< DWORD64 >				loadedModules;
};

/*
========================
Profile_Report
========================
*/
static void Profile_Report() {
	const int numSamples = profileNumSamples;
	if ( numSamples == 0 ) {
		idLib::Printf( "profiler: no samples\n" );
		return;
	}

	idProfileSymbols symbols;

	// resolve every sample to function names once
	std::vector< std::vector< const std::string * > > stacks( numSamples );
	for ( int i = 0; i < numSamples; i++ ) {
		const profileSample_t & sample = profileSamples[i];
		std::vector< const std::string * > & stack = stacks[i];
		stack.reserve( sample.numFrames );
		for ( int f = 0; f < sample.numFrames; f++ ) {
			const DWORD64 addr = ( f == 0 ) ? sample.frames[f] : sample.frames[f] - 1;	// return address -> call site
			stack.push_back( &symbols.Name( addr ) );
		}
	}

	// samples where the thread was just waiting for work aren't interesting
	std::vector< int > busy;
	busy.reserve( numSamples );
	for ( int i = 0; i < numSamples; i++ ) {
		bool idle = false;
		for ( size_t f = 0; f < stacks[i].size() && f < 4; f++ ) {
			const std::string & n = *stacks[i][f];
			if ( n.find( "WaitForSingleObject" ) != std::string::npos || n.find( "WaitForMultipleObjects" ) != std::string::npos
				|| n.find( "NtWaitFor" ) != std::string::npos || n.find( "SleepEx" ) != std::string::npos ) {
				idle = true;
				break;
			}
		}
		if ( !idle ) {
			busy.push_back( i );
		}
	}

	const int seconds = ( ( profileStopTime != 0 ? profileStopTime : Sys_Milliseconds() ) - profileStartTime ) / 1000;
	idLib::Printf( "==================== profiler report ====================\n" );
	idLib::Printf( "%d samples over %d s, %d busy (%.1f%%), percentages are of busy samples\n",
		numSamples, seconds, (int)busy.size(), 100.0f * busy.size() / numSamples );
	if ( busy.empty() ) {
		return;
	}
	const float toPercent = 100.0f / busy.size();
	const int top = Max( 5, sys_profileTop.GetInteger() );

	std::unordered_map< const std::string *, int > self;
	std::unordered_map< const std::string *, int > inclusive;
	std::unordered_map< std::string, int > paths;
	for ( int i : busy ) {
		const std::vector< const std::string * > & stack = stacks[i];
		self[stack[0]]++;

		std::unordered_set< const std::string * > seen;
		for ( const std::string * n : stack ) {
			if ( seen.insert( n ).second ) {
				inclusive[n]++;
			}
		}

		// call path of the 8 innermost functions, outermost first
		std::string path;
		const int depth = Min( (int)stack.size(), 8 );
		for ( int f = depth - 1; f >= 0; f-- ) {
			path += *stack[f];
			if ( f > 0 ) {
				path += "  >  ";
			}
		}
		paths[path]++;
	}

	auto printTop = [&]( const char * title, const auto & counts ) {
		std::vector< std::pair< int, std::string > > sorted;
		for ( const auto & kv : counts ) {
			sorted.emplace_back( kv.second, std::string( *kv.first ) );
		}
		std::sort( sorted.begin(), sorted.end(), []( const auto & a, const auto & b ) { return a.first > b.first; } );
		idLib::Printf( "---- %s ----\n", title );
		for ( int i = 0; i < (int)sorted.size() && i < top; i++ ) {
			idLib::Printf( "%6.2f%% %7d  %s\n", sorted[i].first * toPercent, sorted[i].first, sorted[i].second.c_str() );
		}
	};
	printTop( "self (where the time is actually spent)", self );
	printTop( "inclusive (time spent in the function and its callees)", inclusive );

	std::vector< std::pair< int, std::string > > sortedPaths;
	for ( const auto & kv : paths ) {
		sortedPaths.emplace_back( kv.second, kv.first );
	}
	std::sort( sortedPaths.begin(), sortedPaths.end(), []( const auto & a, const auto & b ) { return a.first > b.first; } );
	idLib::Printf( "---- hottest call paths ----\n" );
	for ( int i = 0; i < (int)sortedPaths.size() && i < top; i++ ) {
		idLib::Printf( "%6.2f%% %7d  %s\n", sortedPaths[i].first * toPercent, sortedPaths[i].first, sortedPaths[i].second.c_str() );
	}
	idLib::Printf( "=========================================================\n" );
}

/*
========================
Sys_ProfilerShutdown
========================
*/
void Sys_ProfilerShutdown() {
	const bool wasRunning = profileRunning != 0;
	Profile_Stop();
	if ( wasRunning ) {
		Profile_Report();
	}
	if ( profileTargetThread != NULL ) {
		CloseHandle( profileTargetThread );
		profileTargetThread = NULL;
		profileTargetThreadId = 0;
	}
	if ( profileSamples != NULL ) {
		VirtualFree( profileSamples, 0, MEM_RELEASE );
		profileSamples = NULL;
	}
}

CONSOLE_COMMAND( profileStart, "start sampling the game thread", 0 ) {
	Profile_Start();
}

CONSOLE_COMMAND( profileStop, "stop sampling and print the profiler report", 0 ) {
	Profile_Stop();
	Profile_Report();
}

CONSOLE_COMMAND( profileReport, "print the profiler report of the samples collected so far", 0 ) {
	Profile_Report();
}
