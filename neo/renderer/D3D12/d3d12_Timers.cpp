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
#include "d3d12_Memory.h"
#include "d3d12_Timers.h"

#include <algorithm>

namespace {

constexpr UINT PIX_EVENT_UNICODE = 0;	// PIX's metadata for a wide string with its terminator
constexpr uint64 NANOSECONDS_PER_SECOND = 1000000000ull;
constexpr uint64 MICROSECONDS_PER_SECOND = 1000000ull;

// Split, so that no product overflows for a clock below 18 GHz.
[[nodiscard]] constexpr uint64 TicksToNanoseconds( uint64 ticks, uint64 frequency ) {
	return ticks / frequency * NANOSECONDS_PER_SECOND + ticks % frequency * NANOSECONDS_PER_SECOND / frequency;
}

}	// namespace

idD3D12GpuTimers::idD3D12GpuTimers( const idD3D12Device & owner, const idD3D12Queue & directQueue ) :
	device( owner ),
	queue( directQueue ) {
	frequency = queue.TimestampFrequency();
	if ( frequency == 0 ) {
		return;
	}
	D3D12_QUERY_HEAP_DESC desc = {};
	desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	desc.Count = SLOTS;
	if ( !device.Check( device.Get().CreateQueryHeap( &desc, IID_PPV_ARGS( &heap ) ), "CreateQueryHeap (timestamps)" ) ) {
		heap.Reset();
		return;
	}
	heap->SetName( L"doom3 timestamps" );
	readback = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_READBACK, SLOTS * sizeof( uint64 ), D3D12_RESOURCE_STATE_COPY_DEST,
		L"doom3 timestamp readback" );
	if ( !readback ) {
		heap.Reset();
	}
}

void idD3D12GpuTimers::BeginFrame( ID3D12GraphicsCommandList1 & list ) {
	if ( !heap ) {
		return;
	}
	list.EndQuery( heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, FRAME_BEGIN );
	frameBegun = true;
}

void idD3D12GpuTimers::Write( ID3D12GraphicsCommandList1 & list, int set, int index ) {
	assert( set >= 0 && set < TIMESTAMP_SETS && index >= 0 && index < SLOTS_PER_SET );
	if ( !heap || set < 0 || set >= TIMESTAMP_SETS || index < 0 || index >= SLOTS_PER_SET ) {
		return;
	}
	list.EndQuery( heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Slot( set, index ) );
	sets[set].written.set( static_cast<size_t>( index ) );
	writes++;
}

// Only what was written is resolved: the layer rejects a query never ended. RenderLog writes the slots
// 0 .. 2n - 1, so a frame resolves one run per set.
void idD3D12GpuTimers::EndFrame( ID3D12GraphicsCommandList1 & list ) {
	if ( !heap ) {
		return;
	}
	const uint64 fence = queue.LastSignaled() + 1;
	if ( frameBegun ) {
		list.EndQuery( heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, FRAME_END );
		Resolve( list, FRAME_BEGIN, 2 );
		frameBegun = false;
		frameFence = fence;
	}
	for ( int s = 0; s < TIMESTAMP_SETS; s++ ) {
		set_t & set = sets[s];
		if ( set.written.none() ) {
			continue;
		}
		for ( int first = 0; first < SLOTS_PER_SET; ) {
			if ( !set.written.test( static_cast<size_t>( first ) ) ) {
				first++;
				continue;
			}
			int end = first + 1;
			while ( end < SLOTS_PER_SET && set.written.test( static_cast<size_t>( end ) ) ) {
				end++;
			}
			Resolve( list, Slot( s, first ), static_cast<UINT>( end - first ) );
			first = end;
		}
		set.resolved = set.written;
		set.written.reset();
		set.fence = fence;
	}
}

void idD3D12GpuTimers::Resolve( ID3D12GraphicsCommandList1 & list, UINT first, UINT count ) const {
	list.ResolveQueryData( heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, count, readback.Get(), first * sizeof( uint64 ) );
}

bool idD3D12GpuTimers::Ready( int set, int index ) const {
	if ( set < 0 || set >= TIMESTAMP_SETS || index < 0 || index >= SLOTS_PER_SET ) {
		return false;
	}
	const set_t & timed = sets[set];
	return timed.fence != 0 && timed.fence <= queue.Waited() && timed.resolved.test( static_cast<size_t>( index ) );
}

void idD3D12GpuTimers::Read( int set, std::span<uint64> nanoseconds ) const {
	std::ranges::fill( nanoseconds, uint64( 0 ) );
	if ( !heap || set < 0 || set >= TIMESTAMP_SETS ) {
		return;
	}
	const set_t & timed = sets[set];
	if ( timed.fence == 0 || timed.fence > queue.Waited() ) {
		return;
	}
	const std::span<uint64> slots = nanoseconds.first( Min( nanoseconds.size(), static_cast<size_t>( SLOTS_PER_SET ) ) );
	if ( !CopyTicks( Slot( set, 0 ), slots ) ) {
		return;
	}
	for ( size_t i = 0; i < slots.size(); i++ ) {
		slots[i] = timed.resolved.test( i ) ? TicksToNanoseconds( slots[i], frequency ) : 0;
	}
}

uint64 idD3D12GpuTimers::FrameMicroseconds() {
	if ( frameFence == 0 || frameFence > queue.Waited() ) {
		return 0;
	}
	if ( frameReadFence != frameFence ) {
		std::array<uint64, 2> ticks = {};
		const bool read = CopyTicks( FRAME_BEGIN, ticks ) && ticks[1] > ticks[0];
		frameMicroseconds = read ? ( ticks[1] - ticks[0] ) * MICROSECONDS_PER_SECOND / frequency : 0;
		frameReadFence = frameFence;
	}
	return frameMicroseconds;
}

void idD3D12GpuTimers::Free() {
	for ( set_t & set : sets ) {
		set = {};
	}
}

// A CPU read of a READBACK buffer maps the range it reads, so that the CPU caches see what the GPU wrote.
bool idD3D12GpuTimers::CopyTicks( UINT first, std::span<uint64> ticks ) const {
	if ( ticks.empty() ) {
		return false;
	}
	const D3D12_RANGE range = { first * sizeof( uint64 ), ( first + ticks.size() ) * sizeof( uint64 ) };
	void * mapped = nullptr;
	if ( !device.Check( readback->Map( 0, &range, &mapped ), "Map (timestamp readback)" ) ) {
		return false;
	}
	memcpy( ticks.data(), static_cast<const byte *>( mapped ) + range.Begin, ticks.size_bytes() );
	const D3D12_RANGE nothingWritten = { 0, 0 };
	readback->Unmap( 0, &nothingWritten );
	return true;
}

void idD3D12DebugGroups::Begin( ID3D12GraphicsCommandList1 & list, int level ) const {
	const std::array<wchar_t, MAX_NAME> & name = names[static_cast<size_t>( level )];
	const size_t length = std::char_traits<wchar_t>::length( name.data() );
	list.BeginEvent( PIX_EVENT_UNICODE, name.data(), static_cast<UINT>( ( length + 1 ) * sizeof( wchar_t ) ) );
}

// Engine names are ASCII: each byte widens to one UTF-16 unit.
void idD3D12DebugGroups::Push( ID3D12GraphicsCommandList1 & list, const char * name ) {
	if ( depth == MAX_DEPTH ) {
		ignored++;
		return;
	}
	std::array<wchar_t, MAX_NAME> & wide = names[static_cast<size_t>( depth )];
	size_t length = 0;
	for ( ; name[length] != '\0' && length + 1 < wide.size(); length++ ) {
		wide[length] = static_cast<wchar_t>( static_cast<unsigned char>( name[length] ) );
	}
	wide[length] = L'\0';
	Begin( list, depth );
	depth++;
	pushed++;
}

void idD3D12DebugGroups::Pop( ID3D12GraphicsCommandList1 * list ) {
	if ( ignored > 0 ) {
		ignored--;
		return;
	}
	if ( depth == 0 ) {
		return;
	}
	depth--;
	if ( list != nullptr ) {
		list->EndEvent();
	}
}

void idD3D12DebugGroups::CloseAll( ID3D12GraphicsCommandList1 & list ) {
	for ( int level = 0; level < depth; level++ ) {
		list.EndEvent();
	}
}

void idD3D12DebugGroups::Reopen( ID3D12GraphicsCommandList1 & list ) {
	for ( int level = 0; level < depth; level++ ) {
		Begin( list, level );
	}
}
