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

#include <intrin.h>
#include <immintrin.h>
#include <vector>

double Sys_GetClockTicks() {
	LARGE_INTEGER li;
	QueryPerformanceCounter( &li );
	return static_cast<double>( li.QuadPart );
}

double Sys_ClockTicksPerSecond() {
	static const double ticks = [] {
		LARGE_INTEGER li;
		QueryPerformanceFrequency( &li );
		return static_cast<double>( li.QuadPart );
	}();
	return ticks;
}

// the nominal clock rate Windows reports, only for the startup log
int Sys_GetCPUMHz() {
	DWORD mhz = 0;
	HKEY key;
	if ( RegOpenKeyExA( HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key ) == ERROR_SUCCESS ) {
		DWORD size = sizeof( mhz );
		if ( RegQueryValueExA( key, "~MHz", nullptr, nullptr, reinterpret_cast<LPBYTE>( &mhz ), &size ) != ERROR_SUCCESS ) {
			mhz = 0;
		}
		RegCloseKey( key );
	}
	return static_cast<int>( mhz );
}

namespace {

struct cpuInfo_t {
	int processorPackageCount;
	int processorCoreCount;
	int logicalProcessorCount;
	int numaNodeCount;
	struct cacheInfo_t {
		int count;
		int associativity;
		int lineSize;
		int size;
	} cacheLevel[3];
};

cpuInfo_t GetCPUInfo() {
	cpuInfo_t cpuInfo = {};

	DWORD length = 0;
	GetLogicalProcessorInformation( nullptr, &length );
	if ( GetLastError() != ERROR_INSUFFICIENT_BUFFER ) {
		idLib::Printf( "GetLogicalProcessorInformation error: %u\n", GetLastError() );
		return cpuInfo;
	}
	std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> infos( length / sizeof( SYSTEM_LOGICAL_PROCESSOR_INFORMATION ) );
	if ( !GetLogicalProcessorInformation( infos.data(), &length ) ) {
		idLib::Printf( "GetLogicalProcessorInformation error: %u\n", GetLastError() );
		return cpuInfo;
	}
	infos.resize( length / sizeof( SYSTEM_LOGICAL_PROCESSOR_INFORMATION ) );

	for ( const SYSTEM_LOGICAL_PROCESSOR_INFORMATION & info : infos ) {
		switch ( info.Relationship ) {
			case RelationProcessorCore:
				cpuInfo.processorCoreCount++;
				// a hyperthreaded core supplies more than one logical processor
				cpuInfo.logicalProcessorCount += static_cast<int>( __popcnt64( info.ProcessorMask ) );
				break;
			case RelationNumaNode:
				cpuInfo.numaNodeCount++;
				break;
			case RelationCache:
				if ( info.Cache.Level >= 1 && info.Cache.Level <= 3 ) {
					cpuInfo_t::cacheInfo_t & cache = cpuInfo.cacheLevel[info.Cache.Level - 1];
					if ( cache.count++ == 0 ) {
						cache.associativity = info.Cache.Associativity;
						cache.lineSize = info.Cache.LineSize;
						cache.size = static_cast<int>( info.Cache.Size );
					}
				}
				break;
			case RelationProcessorPackage:
				cpuInfo.processorPackageCount++;
				break;
			default:
				break;
		}
	}
	return cpuInfo;
}

// the MXCSR mask that FXSAVE stores says whether the DAZ bit is writable
bool HasDAZ() {
	alignas( 16 ) unsigned char area[512] = {};
	_fxsave( area );
	uint32 mask;
	memcpy( &mask, area + 28, sizeof( mask ) );
	return ( mask & ( 1 << 6 ) ) != 0;
}

}

void Sys_GetCPUCacheSize( int level, int & count, int & size, int & lineSize ) {
	assert( level >= 1 && level <= 3 );
	const cpuInfo_t cpuInfo = GetCPUInfo();

	count = cpuInfo.cacheLevel[level - 1].count;
	size = cpuInfo.cacheLevel[level - 1].size;
	lineSize = cpuInfo.cacheLevel[level - 1].lineSize;
}

/*
numLogicalCPUCores	- the number of logical CPU per core
numPhysicalCPUCores	- the total number of cores per package
numCPUPackages		- the total number of packages (physical processors)
*/
void Sys_CPUCount( int & numLogicalCPUCores, int & numPhysicalCPUCores, int & numCPUPackages ) {
	const cpuInfo_t cpuInfo = GetCPUInfo();

	numPhysicalCPUCores = cpuInfo.processorCoreCount;
	numLogicalCPUCores = cpuInfo.logicalProcessorCount;
	numCPUPackages = cpuInfo.processorPackageCount;
}

cpuid_t Sys_GetCPUId() {
	int regs[4];

	__cpuid( regs, 0 );
	char vendor[13] = {};
	memcpy( vendor + 0, &regs[1], 4 );
	memcpy( vendor + 4, &regs[3], 4 );
	memcpy( vendor + 8, &regs[2], 4 );
	int flags = ( idStr::Cmp( vendor, "AuthenticAMD" ) == 0 ) ? CPUID_AMD : CPUID_INTEL;

	__cpuid( regs, 1 );
	const int ecx = regs[2];
	const int edx = regs[3];
	if ( edx & ( 1 << 25 ) ) {
		flags |= CPUID_SSE | CPUID_FTZ;
	}
	if ( edx & ( 1 << 26 ) ) {
		flags |= CPUID_SSE2;
	}
	if ( ecx & ( 1 << 0 ) ) {
		flags |= CPUID_SSE3;
	}

	const cpuInfo_t cpuInfo = GetCPUInfo();
	if ( cpuInfo.logicalProcessorCount > cpuInfo.processorCoreCount ) {
		flags |= CPUID_HTT;
	}

	if ( HasDAZ() ) {
		flags |= CPUID_DAZ;
	}

	return static_cast<cpuid_t>( flags );
}

// fpuExceptions_t follows the MXCSR flag order, the mask bits are the same bits shifted by 7
void Sys_FPU_EnableExceptions( int exceptions ) {
	unsigned int csr = _mm_getcsr();
	csr |= 0x3Fu << 7;
	csr &= ~( ( static_cast<unsigned int>( exceptions ) & 0x3Fu ) << 7 );
	_mm_setcsr( csr );
}

void Sys_FPU_SetDAZ( bool enable ) {
	_MM_SET_DENORMALS_ZERO_MODE( enable ? _MM_DENORMALS_ZERO_ON : _MM_DENORMALS_ZERO_OFF );
}

void Sys_FPU_SetFTZ( bool enable ) {
	_MM_SET_FLUSH_ZERO_MODE( enable ? _MM_FLUSH_ZERO_ON : _MM_FLUSH_ZERO_OFF );
}
