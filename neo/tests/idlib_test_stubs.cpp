/*
===============================================================================

	Engine stubs for idlibTests

	idLib references a few engine globals. The tests don't start the engine,
	so the systems are NULL and only the functions idLib really calls are
	implemented.

===============================================================================
*/

#pragma hdrstop
#include "../idlib/precompiled.h"

#include <windows.h>

idCommon *		common = NULL;
idCVarSystem *	cvarSystem = NULL;
idFileSystem *	fileSystem = NULL;

// cvars constructed before the cvar system exists link themselves into this list
idCVar *		idCVar::staticVars = NULL;

int Sys_Milliseconds() {
	static DWORD start = timeGetTime();
	return (int)( timeGetTime() - start );
}

uint64 Sys_Microseconds() {
	static LARGE_INTEGER frequency = { 0 };
	if ( frequency.QuadPart == 0 ) {
		QueryPerformanceFrequency( &frequency );
	}
	LARGE_INTEGER counter;
	QueryPerformanceCounter( &counter );
	return (uint64)( counter.QuadPart * 1000000 / frequency.QuadPart );
}

void Sys_CPUCount( int & numLogicalCPUCores, int & numPhysicalCPUCores, int & numCPUPackages ) {
	SYSTEM_INFO info;
	GetSystemInfo( &info );
	numLogicalCPUCores = info.dwNumberOfProcessors;
	numPhysicalCPUCores = info.dwNumberOfProcessors;
	numCPUPackages = 1;
}
