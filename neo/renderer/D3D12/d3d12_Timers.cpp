// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Memory.h"
#include "d3d12_Timers.h"

#include <algorithm>
#include <functional>

namespace {

constexpr UINT PIX_EVENT_UNICODE = 0;	// PIX's metadata for a wide string with its terminator
constexpr uint64 NANOSECONDS_PER_SECOND = 1000000000ull;
constexpr uint64 MICROSECONDS_PER_SECOND = 1000000ull;

constexpr int REPORT_MSEC = 1000;

// Split, so that no product overflows for a clock below 18 GHz.
[[nodiscard]] constexpr uint64 TicksToNanoseconds( uint64 ticks, uint64 frequency ) {
	return ticks / frequency * NANOSECONDS_PER_SECOND + ticks % frequency * NANOSECONDS_PER_SECOND / frequency;
}

// A CPU read of a READBACK buffer maps the range it reads, so that the CPU caches see what the GPU wrote.
[[nodiscard]] bool ReadbackCopy( const idD3D12Device & device, ID3D12Resource & readback, UINT first, std::span<uint64> ticks ) {
	if ( ticks.empty() ) {
		return false;
	}
	const D3D12_RANGE range = { first * sizeof( uint64 ), ( first + ticks.size() ) * sizeof( uint64 ) };
	void * mapped = nullptr;
	if ( !device.Check( readback.Map( 0, &range, &mapped ), "Map (timestamp readback)" ) ) {
		return false;
	}
	memcpy( ticks.data(), static_cast<const byte *>( mapped ) + range.Begin, ticks.size_bytes() );
	const D3D12_RANGE nothingWritten = { 0, 0 };
	readback.Unmap( 0, &nothingWritten );
	return true;
}

[[nodiscard]] constexpr UINT PassSlot( int frame, size_t pass ) {
	return static_cast<UINT>( ( static_cast<size_t>( frame ) * idD3D12PassTimer::MAX_PASSES + pass ) * 2 );
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

void idD3D12GpuTimers::BeginFrame( ID3D12GraphicsCommandList4 & list ) {
	if ( !heap ) {
		return;
	}
	list.EndQuery( heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameTimer * 2 );
	frameBegun = true;
}

void idD3D12GpuTimers::Write( ID3D12GraphicsCommandList4 & list, int set, int index ) {
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
void idD3D12GpuTimers::EndFrame( ID3D12GraphicsCommandList4 & list ) {
	if ( !heap ) {
		return;
	}
	const uint64 fence = queue.LastSignaled() + 1;
	if ( frameBegun ) {
		list.EndQuery( heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameTimer * 2 + 1 );
		Resolve( list, frameTimer * 2, 2 );
		frameBegun = false;
		frameFences[frameTimer] = fence;
		frameTimer = ( frameTimer + 1 ) % FRAME_TIMERS;
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

void idD3D12GpuTimers::Resolve( ID3D12GraphicsCommandList4 & list, UINT first, UINT count ) const {
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

// A timer's fence is its last resolve's, so a timer read is never being written.
uint64 idD3D12GpuTimers::FrameMicroseconds() {
	UINT newest = FRAME_TIMERS;
	for ( UINT f = 0; f < FRAME_TIMERS; f++ ) {
		if ( frameFences[f] != 0 && frameFences[f] <= queue.Waited() && ( newest == FRAME_TIMERS || frameFences[f] > frameFences[newest] ) ) {
			newest = f;
		}
	}
	if ( newest == FRAME_TIMERS ) {
		return 0;
	}
	if ( frameReadFence != frameFences[newest] ) {
		std::array<uint64, 2> ticks = {};
		const bool read = CopyTicks( newest * 2, ticks ) && ticks[1] > ticks[0];
		frameMicroseconds = read ? ( ticks[1] - ticks[0] ) * MICROSECONDS_PER_SECOND / frequency : 0;
		frameReadFence = frameFences[newest];
	}
	return frameMicroseconds;
}

void idD3D12GpuTimers::Free() {
	for ( set_t & set : sets ) {
		set = {};
	}
}

bool idD3D12GpuTimers::CopyTicks( UINT first, std::span<uint64> ticks ) const {
	return ReadbackCopy( device, *readback.Get(), first, ticks );
}

// ---- r_pix 4: the graph passes ----------------------------------------------------------------------------

idD3D12PassTimer::idD3D12PassTimer( const idD3D12Device & owner, const idD3D12Queue & directQueue ) :
	device( owner ),
	queue( directQueue ),
	frequency( directQueue.TimestampFrequency() ) {
	if ( frequency == 0 ) {
		return;
	}
	D3D12_QUERY_HEAP_DESC desc = {};
	desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	desc.Count = PassSlot( FRAMES, 0 );
	if ( !device.Check( device.Get().CreateQueryHeap( &desc, IID_PPV_ARGS( &heap ) ), "CreateQueryHeap (pass timestamps)" ) ) {
		heap.Reset();
		return;
	}
	heap->SetName( L"doom3 pass timestamps" );
	readback = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_READBACK, desc.Count * sizeof( uint64 ), D3D12_RESOURCE_STATE_COPY_DEST,
		L"doom3 pass timestamp readback" );
	if ( !readback ) {
		heap.Reset();
	}
}

// A frame still in flight resolves into slots nobody reads again: off forgets it.
void idD3D12PassTimer::Enable( const bool on ) {
	const bool now = on && heap != nullptr;
	if ( now == enabled ) {
		return;
	}
	enabled = now;
	for ( frame_t & frame : frames ) {
		frame = {};
	}
	timing = false;
	depth = 0;
	ResetPeriod();
	periodStart = -1;
}

void idD3D12PassTimer::BeginFrame( const int nowMilliseconds ) {
	timing = false;
	depth = 0;
	if ( !enabled ) {
		return;
	}
	Collect();
	if ( periodStart < 0 ) {
		periodStart = nowMilliseconds;
	} else if ( nowMilliseconds - periodStart >= REPORT_MSEC ) {
		Print( nowMilliseconds - periodStart );
		ResetPeriod();
		periodStart = nowMilliseconds;
	}
	frame_t & frame = frames[current];
	if ( frame.fence != 0 ) {
		periodSkipped++;
		return;
	}
	frame.names.clear();
	timing = true;
}

// A pass recorded inside another one is part of it.
void idD3D12PassTimer::Begin( ID3D12GraphicsCommandList4 & list, const char * name ) {
	if ( !timing || depth++ > 0 ) {
		return;
	}
	frame_t & frame = frames[current];
	if ( frame.names.size() >= MAX_PASSES ) {
		periodUntimed++;
		return;
	}
	list.EndQuery( heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, PassSlot( current, frame.names.size() ) );
	frame.names.push_back( NameId( name ) );
	opened = true;
}

void idD3D12PassTimer::End( ID3D12GraphicsCommandList4 & list ) {
	if ( !timing || depth == 0 || --depth > 0 || !opened ) {
		return;
	}
	list.EndQuery( heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, PassSlot( current, frames[current].names.size() - 1 ) + 1 );
	opened = false;
}

// The resolve goes into the list submitted next, as idD3D12GpuTimers::EndFrame's does.
void idD3D12PassTimer::EndFrame( ID3D12GraphicsCommandList4 & list ) {
	if ( !timing ) {
		return;
	}
	timing = false;
	frame_t & frame = frames[current];
	if ( opened ) {
		list.EndQuery( heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, PassSlot( current, frame.names.size() - 1 ) + 1 );
		opened = false;
	}
	depth = 0;
	if ( frame.names.empty() ) {
		return;
	}
	const UINT first = PassSlot( current, 0 );
	list.ResolveQueryData( heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, static_cast<UINT>( frame.names.size() * 2 ), readback.Get(),
		first * sizeof( uint64 ) );
	frame.fence = queue.LastSignaled() + 1;
	current = ( current + 1 ) % FRAMES;
}

void idD3D12PassTimer::Collect() {
	const uint64 waited = queue.Waited();
	for ( int f = 0; f < FRAMES; f++ ) {
		frame_t & frame = frames[f];
		if ( frame.fence == 0 || frame.fence > waited ) {
			continue;
		}
		ticks.resize( frame.names.size() * 2 );
		if ( ReadbackCopy( device, *readback.Get(), PassSlot( f, 0 ), ticks ) ) {
			frameMs.assign( names.size(), 0.0 );
			frameCount.assign( names.size(), 0 );
			for ( size_t p = 0; p < frame.names.size(); p++ ) {
				const uint64 begin = ticks[p * 2];
				const uint64 end = ticks[p * 2 + 1];
				frameMs[frame.names[p]] += ( end > begin ) ? static_cast<double>( end - begin ) * 1000.0 / static_cast<double>( frequency ) : 0.0;
				frameCount[frame.names[p]]++;
			}
			period.resize( names.size() );
			for ( size_t id = 0; id < names.size(); id++ ) {
				if ( frameCount[id] > 0 ) {
					period[id].sum += frameMs[id];
					period[id].max = Max( period[id].max, frameMs[id] );
					period[id].count += frameCount[id];
				}
			}
			periodFrames++;
		}
		frame = {};
	}
}

std::vector<idD3D12PassTimer::passTime_t> idD3D12PassTimer::PeriodTimes() const {
	std::vector<passTime_t> times;
	if ( periodFrames == 0 ) {
		return times;
	}
	const double scale = 1.0 / periodFrames;
	for ( size_t id = 0; id < period.size(); id++ ) {
		if ( period[id].count > 0 ) {
			times.push_back( { names[id].c_str(), period[id].sum * scale, period[id].max, period[id].count * scale } );
		}
	}
	std::ranges::stable_sort( times, std::greater{}, &passTime_t::gpuMilliseconds );
	return times;
}

// Passes named "<family>: <pass>" (NRD's) are summed per family after the list.
void idD3D12PassTimer::Print( const int periodMilliseconds ) const {
	if ( periodFrames == 0 ) {
		if ( periodSkipped > 0 ) {
			common->Printf( "r_pix 4: no pass times came back from the GPU in %.2f s, %d frames not timed\n", periodMilliseconds * 0.001, periodSkipped );
		}
		return;
	}
	common->Printf( "----- r_pix 4: GPU ms per frame of the graph passes, average and max of %d frames in %.2f s -----\n", periodFrames,
		periodMilliseconds * 0.001 );
	common->Printf( "%-56s %6s %8s %8s\n", "pass", "count", "GPU avg", "GPU max" );
	std::vector<std::pair<std::string_view, double>> families;
	double total = 0.0;
	for ( const passTime_t & time : PeriodTimes() ) {
		common->Printf( "%-56s %6.1f %8.3f %8.3f\n", time.name, time.count, time.gpuMilliseconds, time.maxMilliseconds );
		total += time.gpuMilliseconds;
		const std::string_view name( time.name );
		const size_t colon = name.find( ':' );
		if ( colon == std::string_view::npos ) {
			continue;
		}
		const std::string_view family = name.substr( 0, colon );
		const auto known = std::ranges::find( families, family, &std::pair<std::string_view, double>::first );
		if ( known == families.end() ) {
			families.emplace_back( family, time.gpuMilliseconds );
		} else {
			known->second += time.gpuMilliseconds;
		}
	}
	for ( const auto & [family, milliseconds] : families ) {
		common->Printf( "%-56s %6s %8.3f\n", ( std::string( family ) + " (all)" ).c_str(), "", milliseconds );
	}
	common->Printf( "%-56s %6s %8.3f\n", "all graph passes", "", total );
	if ( periodSkipped > 0 ) {
		common->Printf( "%d frames not timed, their slots were in flight\n", periodSkipped );
	}
	if ( periodUntimed > 0 ) {
		common->Printf( "%d passes not timed, over %d per frame\n", periodUntimed, MAX_PASSES );
	}
}

void idD3D12PassTimer::ResetPeriod() {
	for ( stats_t & stats : period ) {
		stats = {};
	}
	periodFrames = 0;
	periodSkipped = 0;
	periodUntimed = 0;
}

uint16 idD3D12PassTimer::NameId( const char * name ) {
	const std::string_view key = ( name != nullptr && name[0] != '\0' ) ? name : "?";
	if ( const auto known = nameIds.find( key ); known != nameIds.end() ) {
		return known->second;
	}
	const uint16 id = static_cast<uint16>( names.size() );
	names.emplace_back( key );
	nameIds.emplace( names.back(), id );
	return id;
}

void idD3D12DebugGroups::Begin( ID3D12GraphicsCommandList4 & list, int level ) const {
	const std::array<wchar_t, MAX_NAME> & name = names[static_cast<size_t>( level )];
	const size_t length = std::char_traits<wchar_t>::length( name.data() );
	list.BeginEvent( PIX_EVENT_UNICODE, name.data(), static_cast<UINT>( ( length + 1 ) * sizeof( wchar_t ) ) );
}

// Engine names are ASCII: each byte widens to one UTF-16 unit.
void idD3D12DebugGroups::Push( ID3D12GraphicsCommandList4 & list, const char * name ) {
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

void idD3D12DebugGroups::Pop( ID3D12GraphicsCommandList4 * list ) {
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

void idD3D12DebugGroups::CloseAll( ID3D12GraphicsCommandList4 & list ) {
	for ( int level = 0; level < depth; level++ ) {
		list.EndEvent();
	}
}

void idD3D12DebugGroups::Reopen( ID3D12GraphicsCommandList4 & list ) {
	for ( int level = 0; level < depth; level++ ) {
		Begin( list, level );
	}
}
