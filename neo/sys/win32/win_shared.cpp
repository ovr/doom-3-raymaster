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
#include "../../idlib/precompiled.h"

#include "win_local.h"
#include <lmerr.h>
#include <lmcons.h>
#include <lmwksta.h>
#include <errno.h>
#include <fcntl.h>
#include <direct.h>
#include <io.h>
#include <conio.h>
#undef StrCmpN
#undef StrCmpNI
#undef StrCmpI

#include <comdef.h>
#include <comutil.h>
#include <Wbemidl.h>

#pragma comment (lib, "wbemuuid.lib")

// COM smart pointers from the compiler support library (comdef.h), so ATL is not required
typedef _com_ptr_t< _com_IIID< IWbemLocator, &__uuidof( IWbemLocator ) > >					wbemLocatorPtr_t;
typedef _com_ptr_t< _com_IIID< IWbemServices, &__uuidof( IWbemServices ) > >				wbemServicesPtr_t;
typedef _com_ptr_t< _com_IIID< IEnumWbemClassObject, &__uuidof( IEnumWbemClassObject ) > >	wbemEnumClassObjectPtr_t;
typedef _com_ptr_t< _com_IIID< IWbemClassObject, &__uuidof( IWbemClassObject ) > >			wbemClassObjectPtr_t;

#pragma warning(disable:4740)	// warning C4740: flow in or out of inline asm code suppresses global optimization

/*
================
Sys_Milliseconds
================
*/
int Sys_Milliseconds() {
	static DWORD sys_timeBase = timeGetTime();
	return timeGetTime() - sys_timeBase;
}

/*
========================
Sys_Microseconds
========================
*/
uint64 Sys_Microseconds() {
	// QPC, not the TSC scaled by the registry's nominal ~MHz: that one ran off real time by up to
	// about 1 %, and the frame loop now times the game tics with this clock
	static const uint64 frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency( &f ); return static_cast<uint64>( f.QuadPart ); }();
	LARGE_INTEGER counter;
	QueryPerformanceCounter( &counter );
	const uint64 ticks = static_cast<uint64>( counter.QuadPart );
	return ( ticks / frequency ) * 1000000 + ( ticks % frequency ) * 1000000 / frequency;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION	0x00000002
#endif

/*
========================
Sys_WaitUntilMicroseconds
========================
*/
void Sys_WaitUntilMicroseconds( const uint64 targetMicroseconds ) {
	// a high-resolution timer (Windows 10 1803+) wakes within about 0.5 ms, Sleep under
	// timeBeginPeriod( 1 ) within about 2 ms; the rest is spun on the clock
	thread_local const HANDLE timer = CreateWaitableTimerExW( nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS );
	const uint64 spinMicroseconds = ( timer != nullptr ) ? 1000 : 2000;

	for ( uint64 now = Sys_Microseconds(); now + spinMicroseconds < targetMicroseconds; now = Sys_Microseconds() ) {
		const uint64 sleepMicroseconds = targetMicroseconds - spinMicroseconds - now;
		LARGE_INTEGER dueTime;
		dueTime.QuadPart = -static_cast<LONGLONG>( sleepMicroseconds * 10 );	// relative, in 100 ns units
		if ( timer == nullptr || !SetWaitableTimer( timer, &dueTime, 0, nullptr, nullptr, FALSE ) || WaitForSingleObject( timer, INFINITE ) != WAIT_OBJECT_0 ) {
			Sleep( static_cast<DWORD>( sleepMicroseconds / 1000 ) );
		}
	}
	while ( Sys_Microseconds() < targetMicroseconds ) {
		YieldProcessor();
	}
}

/*
================
Sys_GetSystemRam

	returns amount of physical memory in MB
================
*/
int Sys_GetSystemRam() {
	MEMORYSTATUSEX statex;
	statex.dwLength = sizeof ( statex );
	GlobalMemoryStatusEx (&statex);
	int physRam = statex.ullTotalPhys / ( 1024 * 1024 );
	// HACK: For some reason, ullTotalPhys is sometimes off by a meg or two, so we round up to the nearest 16 megs
	physRam = ( physRam + 8 ) & ~15;
	return physRam;
}


/*
================
Sys_GetDriveFreeSpace
returns in megabytes
================
*/
int Sys_GetDriveFreeSpace( const char *path ) {
	DWORDLONG lpFreeBytesAvailable;
	DWORDLONG lpTotalNumberOfBytes;
	DWORDLONG lpTotalNumberOfFreeBytes;
	int ret = 26;
	//FIXME: see why this is failing on some machines
	if ( ::GetDiskFreeSpaceEx( path, (PULARGE_INTEGER)&lpFreeBytesAvailable, (PULARGE_INTEGER)&lpTotalNumberOfBytes, (PULARGE_INTEGER)&lpTotalNumberOfFreeBytes ) ) {
		ret = ( double )( lpFreeBytesAvailable ) / ( 1024.0 * 1024.0 );
	}
	return ret;
}

/*
========================
Sys_GetDriveFreeSpaceInBytes
========================
*/
int64 Sys_GetDriveFreeSpaceInBytes( const char * path ) {
	DWORDLONG lpFreeBytesAvailable;
	DWORDLONG lpTotalNumberOfBytes;
	DWORDLONG lpTotalNumberOfFreeBytes;
	int64 ret = 1;
	//FIXME: see why this is failing on some machines
	if ( ::GetDiskFreeSpaceEx( path, (PULARGE_INTEGER)&lpFreeBytesAvailable, (PULARGE_INTEGER)&lpTotalNumberOfBytes, (PULARGE_INTEGER)&lpTotalNumberOfFreeBytes ) ) {
		ret = lpFreeBytesAvailable;
	}
	return ret;
}

/*
================
Sys_GetVideoRam
returns in megabytes
================
*/
int Sys_GetVideoRam() {
	unsigned int retSize = 64;

	wbemLocatorPtr_t spLoc;
	HRESULT hr = CoCreateInstance( CLSID_WbemLocator, 0, CLSCTX_SERVER, IID_IWbemLocator, ( LPVOID * ) &spLoc );
	if ( hr != S_OK || spLoc == NULL ) {
		return retSize;
	}

	_bstr_t bstrNamespace( L"\\\\.\\root\\CIMV2" );
	wbemServicesPtr_t spServices;

	// Connect to CIM
	hr = spLoc->ConnectServer( bstrNamespace, NULL, NULL, 0, NULL, 0, 0, &spServices );
	if ( hr != WBEM_S_NO_ERROR ) {
		return retSize;
	}

	// Switch the security level to IMPERSONATE so that provider will grant access to system-level objects.  
	hr = CoSetProxyBlanket( spServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE );
	if ( hr != S_OK ) {
		return retSize;
	}

	// Get the vid controller
	wbemEnumClassObjectPtr_t spEnumInst;
	hr = spServices->CreateInstanceEnum( _bstr_t( L"Win32_VideoController" ), WBEM_FLAG_SHALLOW, NULL, &spEnumInst );
	if ( hr != WBEM_S_NO_ERROR || spEnumInst == NULL ) {
		return retSize;
	}

	ULONG uNumOfInstances = 0;
	wbemClassObjectPtr_t spInstance;
	hr = spEnumInst->Next( 10000, 1, &spInstance, &uNumOfInstances );

	if ( hr == S_OK && spInstance ) {
		// Get properties from the object
		_variant_t varSize;
		hr = spInstance->Get( L"AdapterRAM", 0, &varSize, 0, 0 );
		if ( hr == S_OK ) {
			retSize = varSize.intVal / ( 1024 * 1024 );
			if ( retSize == 0 ) {
				retSize = 64;
			}
		}
	}
	return retSize;
}

/*
================
Sys_GetCurrentMemoryStatus

	returns OS mem info
	all values are in MB except the memoryload
================
*/
void Sys_GetCurrentMemoryStatus( sysMemoryStats_t &stats ) {
	MEMORYSTATUSEX statex = {};
	statex.dwLength = sizeof( statex );
	GlobalMemoryStatusEx( &statex );

	const auto megabytes = []( DWORDLONG bytes ) { return static_cast<int64>( bytes >> 20 ); };
	stats = {};
	stats.memoryLoad = static_cast<int>( statex.dwMemoryLoad );
	stats.totalPhysical = megabytes( statex.ullTotalPhys );
	stats.availPhysical = megabytes( statex.ullAvailPhys );
	stats.totalPageFile = megabytes( statex.ullTotalPageFile );
	stats.availPageFile = megabytes( statex.ullAvailPageFile );
	stats.totalVirtual = megabytes( statex.ullTotalVirtual );
	stats.availVirtual = megabytes( statex.ullAvailVirtual );
	stats.availExtendedVirtual = megabytes( statex.ullAvailExtendedVirtual );
}

/*
================
Sys_LockMemory
================
*/
bool Sys_LockMemory( void *ptr, int bytes ) {
	return ( VirtualLock( ptr, (SIZE_T)bytes ) != FALSE );
}

/*
================
Sys_UnlockMemory
================
*/
bool Sys_UnlockMemory( void *ptr, int bytes ) {
	return ( VirtualUnlock( ptr, (SIZE_T)bytes ) != FALSE );
}

/*
================
Sys_SetPhysicalWorkMemory
================
*/
void Sys_SetPhysicalWorkMemory( int minBytes, int maxBytes ) {
	::SetProcessWorkingSetSize( GetCurrentProcess(), minBytes, maxBytes );
}

/*
================
Sys_GetCurrentUser
================
*/
char *Sys_GetCurrentUser() {
	static char s_userName[1024];
	unsigned long size = sizeof( s_userName );


	if ( !GetUserName( s_userName, &size ) ) {
		strcpy( s_userName, "player" );
	}

	if ( !s_userName[0] ) {
		strcpy( s_userName, "player" );
	}

	return s_userName;
}
