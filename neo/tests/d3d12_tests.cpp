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
#include "../idlib/precompiled.h"

#include "../renderer/tr_local.h"
#include "../renderer/D3D12/d3d12_Buffers.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <thread>
#include <vector>

/*
	d3d12Tests: the parts of the Direct3D 12 backend that run without a window. The ring and the handle
	table are pure arithmetic; the uploader and the buffer store run on a WARP device with the debug layer.

	Usage: d3d12Tests [filter]     filter: run the tests whose name contains it; "all" runs every test
	D3D12_TESTS_VERBOSE=1 prints every failed check, not only the first 20 of a test.
*/

namespace {

int		numChecks = 0;
int		numFailures = 0;
int		numFailuresInTest = 0;
int		maxFailuresPerTest = 20;

bool Check( bool condition, const char * what, const std::source_location where = std::source_location::current() ) {
	numChecks++;
	if ( condition ) {
		return true;
	}
	numFailures++;
	numFailuresInTest++;
	if ( numFailuresInTest <= maxFailuresPerTest ) {
		printf( "  FAIL line %u: %s\n", static_cast<unsigned>( where.line() ), what );
	}
	return false;
}

[[nodiscard]] std::vector<byte> Pattern( size_t bytes, uint32 seed ) {
	std::vector<byte> data( bytes );
	uint32 x = seed;
	for ( byte & b : data ) {
		x = x * 1664525u + 1013904223u;
		b = static_cast<byte>( x >> 24 );
	}
	return data;
}

// ---- the ring ----------------------------------------------------------------------------------------------

void Test_RingWrap() {
	idD3D12RingAllocator ring( 1000 );
	Check( ring.TryAllocate( 400, 1 ) == 0u, "the first allocation is at 0" );
	ring.CloseChunk( 1 );
	Check( ring.TryAllocate( 400, 1 ) == 400u, "the second follows it" );
	ring.CloseChunk( 2 );
	Check( !ring.TryAllocate( 300, 1 ), "200 bytes at the end and nothing retired: no room" );
	ring.Retire( 1 );
	Check( ring.TryAllocate( 300, 1 ) == 0u, "wraps to 0 instead of splitting" );
	Check( ring.Used() == 900, va( "used %llu: the skipped 200 bytes count", static_cast<unsigned long long>( ring.Used() ) ) );
	ring.CloseChunk( 3 );
	Check( ring.OldestFence() == 2, "fence order" );
	ring.Retire( 3 );
	Check( ring.Used() == 0 && ring.OldestFence() == 0, "every chunk retired" );
}

void Test_RingRelease() {
	idD3D12RingAllocator ring( 1024 );
	Check( ring.TryAllocate( 100, 1 ) == 0u, "first" );
	ring.CloseChunk( 5 );
	Check( ring.TryAllocate( 100, 1 ) == 100u, "second" );
	ring.CloseChunk( 6 );
	ring.Retire( 4 );
	Check( ring.Used() == 200 && ring.OldestFence() == 5, "an earlier fence frees nothing" );
	ring.Retire( 5 );
	Check( ring.Used() == 100 && ring.OldestFence() == 6, "fence 5 frees its chunk only" );
	Check( ring.TryAllocate( 50, 1 ) == 200u, "an open allocation" );
	ring.Retire( 100 );
	Check( ring.Used() == 50 && ring.OldestFence() == 0, "an open allocation is no chunk yet: it stays" );
	ring.CloseChunk( 7 );
	ring.Retire( 7 );
	Check( ring.Used() == 0, "empty" );
	Check( ring.TryAllocate( 1024, 1 ) == 0u, "an empty ring starts over: its whole length is free" );
}

void Test_RingFull() {
	idD3D12RingAllocator ring( 256 );
	Check( !ring.TryAllocate( 257, 1 ), "more than the ring" );
	Check( ring.TryAllocate( 256, 1 ) == 0u, "the whole ring" );
	Check( !ring.TryAllocate( 1, 1 ), "full" );
	ring.CloseChunk( 9 );
	ring.Retire( 8 );
	Check( !ring.TryAllocate( 1, 1 ), "full until fence 9 is waited for" );
	ring.Retire( 9 );
	Check( ring.TryAllocate( 10, 1 ) == 0u, "free again" );
	Check( ring.TryAllocate( 10, 64 ) == 64u, "aligned" );
	Check( ring.Used() == 74, "the alignment gap counts" );
}

// ---- the handle table ----------------------------------------------------------------------------------------

enum class testHandle_t : uint64 { NONE = 0 };
using testTable_t = idD3D12HandleTable<int, testHandle_t>;

[[nodiscard]] constexpr uint64 Bits( testHandle_t handle ) { return static_cast<uint64>( handle ); }
[[nodiscard]] constexpr uint64 IndexOf( testHandle_t handle ) { return ( Bits( handle ) & 0xFFFFFFFFull ) - 1; }

void Test_HandleGenerations() {
	testTable_t table;
	const testHandle_t a = table.Add( 10 );
	Check( Bits( a ) == ( ( 1ull << 32 ) | 1 ), "index 0, generation 1" );
	Check( table.Find( a ) != nullptr && *table.Find( a ) == 10, "Find a live handle" );
	Check( table.Remove( a ) == 10, "Remove gives the record back" );
	Check( table.Find( a ) == nullptr, "a removed handle is dead" );
	const testHandle_t b = table.Add( 20 );
	Check( Bits( b ) == ( ( 2ull << 32 ) | 1 ), "the slot is reused with generation 2" );
	Check( table.Find( a ) == nullptr, "the old handle stays dead after the reuse" );
	Check( table.Get( b ) == 20 && table.Num() == 1, "Get and Num" );
}

void Test_HandleFindDead() {
	testTable_t table;
	Check( table.Find( testHandle_t::NONE ) == nullptr, "NONE" );
	const testHandle_t a = table.Add( 1 );
	Check( table.Find( static_cast<testHandle_t>( Bits( a ) + ( 1ull << 32 ) ) ) == nullptr, "a later generation" );
	Check( table.Find( static_cast<testHandle_t>( Bits( a ) + 1 ) ) == nullptr, "an index never used" );
	Check( table.Find( static_cast<testHandle_t>( ( 1ull << 32 ) | 257 ) ) == nullptr, "a chunk never allocated" );
	Check( table.Find( static_cast<testHandle_t>( ( 1ull << 32 ) | 65537 ) ) == nullptr, "an index beyond the table" );
	Check( table.Find( static_cast<testHandle_t>( 1ull << 32 ) ) == nullptr, "index + 1 = 0 is no index" );
	const testTable_t & constTable = table;
	Check( constTable.Find( a ) != nullptr && *constTable.Find( a ) == 1, "const Find" );
}

void Test_HandleLowestFree() {
	testTable_t table;
	std::array<testHandle_t, 5> handles = {};
	for ( int i = 0; i < 5; i++ ) {
		handles[i] = table.Add( int( i ) );
	}
	Check( table.Remove( handles[3] ) == 3 && table.Remove( handles[1] ) == 1, "Remove" );
	const testHandle_t c = table.Add( 100 );
	const testHandle_t d = table.Add( 101 );
	const testHandle_t e = table.Add( 102 );
	Check( IndexOf( c ) == 1 && IndexOf( d ) == 3 && IndexOf( e ) == 5, "the lowest free index first: handles repeat run to run" );
	std::vector<int> order;
	table.ForEach( [&order]( testHandle_t, int & value ) { order.push_back( value ); } );
	Check( order == std::vector<int>{ 0, 100, 2, 101, 4, 102 }, "ForEach in index order" );
}

int destroyedRecords = 0;
struct countedRecord_t {
	~countedRecord_t() { destroyedRecords++; }
};

void Test_HandleClear() {
	idD3D12HandleTable<std::unique_ptr<countedRecord_t>, testHandle_t> table;
	destroyedRecords = 0;
	std::array<testHandle_t, 10> handles = {};
	for ( testHandle_t & handle : handles ) {
		handle = table.Add( std::make_unique<countedRecord_t>() );
	}
	{
		const std::unique_ptr<countedRecord_t> removed = table.Remove( handles[4] );
		Check( destroyedRecords == 0, "Remove moves the record out" );
	}
	Check( destroyedRecords == 1, "the removed record dies with its owner" );
	table.Clear();
	Check( destroyedRecords == 10 && table.Num() == 0, "Clear destroys every live record" );
	bool anyLive = false;
	for ( const testHandle_t handle : handles ) {
		anyLive |= table.Find( handle ) != nullptr;
	}
	Check( !anyLive, "every handle is dead after Clear" );
	const testHandle_t again = table.Add( std::make_unique<countedRecord_t>() );
	Check( IndexOf( again ) == 0 && again != handles[0], "index 0 again, a new generation" );
}

void Test_HandleFull() {
	idD3D12HandleTable<int, testHandle_t, 256> table;
	bool allIssued = true;
	for ( int i = 0; i < 256; i++ ) {
		allIssued &= table.Add( int( i ) ) != testHandle_t::NONE;
	}
	Check( allIssued && table.Num() == 256, "256 records" );
	Check( table.Add( 256 ) == testHandle_t::NONE, "a full table gives NONE" );
}

// MapBuffer: the vertex cache maps its static set from the game thread while the main thread adds buffers.
void Test_HandleFindDuringAdd() {
	const auto table = std::make_unique<testTable_t>();
	const testHandle_t watched = table->Add( 42 );
	std::atomic<bool> stop = false;
	std::atomic<int> finds = 0;
	std::atomic<int> wrong = 0;
	std::thread reader( [&]() {
		const testTable_t & readOnly = *table;
		while ( !stop.load( std::memory_order_acquire ) ) {
			const int * value = readOnly.Find( watched );
			if ( value == nullptr || *value != 42 ) {
				wrong++;
			}
			finds++;
		}
	} );
	while ( finds.load() == 0 ) {
		std::this_thread::yield();
	}
	int notIssued = 0;
	for ( int i = 0; i < 60000; i++ ) {
		notIssued += ( table->Add( int( i ) ) == testHandle_t::NONE ) ? 1 : 0;
	}
	stop.store( true, std::memory_order_release );
	reader.join();
	Check( notIssued == 0 && table->Num() == 60001, "60000 adds into 235 chunks" );
	Check( wrong.load() == 0, va( "%d of %d Finds from the other thread failed", wrong.load(), finds.load() ) );
}

// ---- WARP: the uploader and the buffer store ----------------------------------------------------------------

// The context's submission without a window: one list, the staging ring and the graveyard, retired by
// explicit waits only.
class idTestGpu final : public idD3D12Submitter {
public:
	idTestGpu( idD3D12Library && loadedLibrary, idD3D12Device && createdDevice, uint64 stagingSize ) :
		library( std::move( loadedLibrary ) ),
		device( std::move( createdDevice ) ),
		liveObjects( device ),
		queue( device ),
		graveyard( queue ),
		commands( device, queue ),
		barriers( *this ),
		staging( device, stagingSize, L"test staging ring" ),
		uploader( device, *this, staging, barriers, graveyard ),
		buffers( device, graveyard, uploader, barriers, d3d12BufferPlacement_t::UPLOAD ) {
	}
	~idTestGpu() {
		if ( !device.IsLost() ) {
			WaitIdle();
		}
		buffers.DestroyAll();
		graveyard.CollectAll();
	}
	idTestGpu( const idTestGpu & ) = delete;
	idTestGpu & operator=( const idTestGpu & ) = delete;

	ID3D12GraphicsCommandList1 & List() override { return commands.IsOpen() ? commands.List() : commands.Open(); }
	uint64 SubmitChunk() override {
		if ( !commands.IsOpen() ) {
			return queue.LastSignaled();
		}
		barriers.Flush();
		const uint64 fence = commands.CloseAndExecute();
		staging.CloseChunk( fence );
		submits++;
		return fence;
	}
	void WaitFor( uint64 fence ) override {
		waits += ( fence > queue.Waited() ) ? 1 : 0;
		queue.Wait( fence );
		staging.Retire( queue.Waited() );
		graveyard.Collect();
	}
	void WaitIdle() override {
		SubmitChunk();
		WaitFor( queue.Signal() );
	}
	uint64 PendingFence() const override { return queue.LastSignaled() + 1; }
	uint64 ListSerial() const override { return commands.IsOpen() ? commands.Serial() : commands.Serial() + 1; }
	uint64 WaitedFence() const override { return queue.Waited(); }

	// bytes of resource at offset through a READBACK buffer; state nullptr = an UPLOAD buffer, GENERIC_READ for good.
	[[nodiscard]] std::vector<byte> Read( ID3D12Resource & resource, d3d12ResourceState_t * state, uint64 offset, uint64 bytes );
	// Blocks until the GPU has run everything submitted, behind the queue's back: Waited() does not move.
	void FinishUnwaited();

	idD3D12Library					library;
	idD3D12Device					device;
	idD3D12LiveObjectReport			liveObjects;
	idD3D12Queue					queue;
	idD3D12Graveyard				graveyard;
	idD3D12CommandStream			commands;
	idD3D12Barriers					barriers;
	idD3D12UploadRing				staging;
	idD3D12Uploader					uploader;
	idD3D12BufferStore				buffers;
	int								submits = 0;
	int								waits = 0;
};

std::vector<byte> idTestGpu::Read( ID3D12Resource & resource, d3d12ResourceState_t * state, uint64 offset, uint64 bytes ) {
	idComPtr<ID3D12Resource> readback = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
		L"test readback" );
	if ( !readback ) {
		return {};
	}
	if ( state != nullptr ) {
		barriers.Transition( resource, *state, D3D12_RESOURCE_STATE_COPY_SOURCE );
		barriers.Flush();
	}
	List().CopyBufferRegion( readback.Get(), 0, &resource, offset, bytes );
	WaitIdle();
	std::vector<byte> data( static_cast<size_t>( bytes ) );
	const D3D12_RANGE everything = { 0, static_cast<SIZE_T>( bytes ) };
	void * mapped = nullptr;
	if ( device.Check( readback->Map( 0, &everything, &mapped ), "Map (test readback)" ) ) {
		memcpy( data.data(), mapped, data.size() );
		const D3D12_RANGE nothingWritten = { 0, 0 };
		readback->Unmap( 0, &nothingWritten );
	}
	return data;
}

void idTestGpu::FinishUnwaited() {
	idComPtr<ID3D12Fence> fence;
	if ( !device.Check( device.Get().CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( &fence ) ), "CreateFence (test)" ) ) {
		return;
	}
	device.Check( queue.Get().Signal( fence.Get(), 1 ), "Signal (test)" );
	device.Check( fence->SetEventOnCompletion( 1, nullptr ), "SetEventOnCompletion (test)" );	// no event: blocks
}

[[nodiscard]] std::unique_ptr<idTestGpu> CreateWarp( uint64 stagingSize ) {
	auto library = idD3D12Library::Load();
	if ( !library ) {
		Check( false, va( "idD3D12Library::Load: %s", library.error().c_str() ) );
		return nullptr;
	}
	auto device = idD3D12Device::Create( *library, "warp", 1 );
	if ( !device ) {
		Check( false, va( "idD3D12Device::Create( warp ): %s", device.error().c_str() ) );
		return nullptr;
	}
	if ( device->DebugLevel() == 0 ) {
		printf( "  no debug layer (Graphics Tools): the API usage is not validated\n" );
	}
	return std::make_unique<idTestGpu>( std::move( *library ), std::move( *device ), stagingSize );
}

void CheckNoLayerErrors( const idTestGpu & gpu ) {
	const int errors = gpu.device.DrainMessages();
	Check( errors == 0, va( "%d debug layer errors", errors ) );
}

// 50 MB through a 40 MB ring: Room waits for the oldest piece and Pace submits every 16 MB.
void Test_WarpStaticBuffer() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 40 << 20 );
	if ( !gpu ) {
		return;
	}
	constexpr uint64 SIZE = 50 << 20;
	std::vector<byte> data = Pattern( SIZE, 1 );
	const bufferHandle_t handle = gpu->buffers.Create( bufferType_t::VERTEX, bufferUsage_t::STATIC, static_cast<int>( SIZE ), data.data() );
	if ( !Check( handle != bufferHandle_t::NONE, "Create" ) ) {
		return;
	}
	const d3d12Buffer_t & buffer = gpu->buffers.Get( handle );
	Check( buffer.placement == d3d12BufferPlacement_t::STAGED && buffer.bytes == SIZE, "STATIC is staged" );
	Check( gpu->uploader.BytesThisFrame() == SIZE, "the data went through the staging ring" );
	Check( gpu->submits >= 3, va( "%d submits: one every 16 MB", gpu->submits ) );
	Check( gpu->waits >= 1, va( "%d waits: the ring waited for its oldest piece", gpu->waits ) );
	Check( gpu->buffers.Bytes( bufferUsage_t::STATIC ) == SIZE && gpu->buffers.MirrorBytes() == SIZE, "the byte counts" );

	gpu->WaitIdle();
	d3d12ResourceState_t state;
	state.decays = true;
	Check( gpu->Read( *buffer.resource.Get(), &state, 0, SIZE ) == data, "the GPU copy equals the data" );

	byte * mapped = static_cast<byte *>( gpu->buffers.Map( handle ) );
	Check( mapped == buffer.cpu, "Map gives the mirror" );
	for ( int i = 0; i < 4096; i++ ) {
		mapped[1000 + i] = data[1000 + i] = static_cast<byte>( i * 7 );
	}
	gpu->uploader.ResetFrameCounters();
	gpu->buffers.Unmap( handle, 1000, 4096 );
	Check( gpu->uploader.BytesThisFrame() == 4096, "Unmap copies the dirty range only" );
	gpu->WaitIdle();
	const std::vector<byte> head = gpu->Read( *buffer.resource.Get(), &state, 0, 8192 );
	Check( head.size() == 8192 && std::equal( head.begin(), head.end(), data.begin() ), "the dirty range reached the GPU" );

	gpu->buffers.Destroy( handle );
	Check( gpu->buffers.Num() == 0 && gpu->buffers.Bytes( bufferUsage_t::STATIC ) == 0 && gpu->buffers.MirrorBytes() == 0, "Destroy" );
	CheckNoLayerErrors( *gpu );
}

void Test_WarpFrameBuffers() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 );
	if ( !gpu ) {
		return;
	}
	gpu->buffers.CreateDefaults();
	Check( gpu->buffers.ZeroJoints() != 0, "the zero joints" );
	const D3D12_VERTEX_BUFFER_VIEW attribs = gpu->buffers.DefaultAttribs();
	Check( attribs.BufferLocation != 0 && attribs.SizeInBytes == 16 && attribs.StrideInBytes == 0, "the default attribute: stride 0" );

	std::vector<byte> data = Pattern( 1024, 2 );
	const bufferHandle_t handle = gpu->buffers.Create( bufferType_t::JOINT, bufferUsage_t::FRAME, 1024, data.data() );
	if ( !Check( handle != bufferHandle_t::NONE, "Create" ) ) {
		return;
	}
	const d3d12Buffer_t & buffer = gpu->buffers.Get( handle );
	constexpr uint64 BYTES = 1024 + idD3D12BufferStore::JOINT_TAIL;
	Check( buffer.placement == d3d12BufferPlacement_t::UPLOAD && buffer.bytes == BYTES, "FRAME is an UPLOAD buffer with the joint tail" );
	Check( gpu->buffers.Bytes( bufferUsage_t::FRAME ) == BYTES, "the byte count" );
	Check( gpu->uploader.BytesThisFrame() == 0 && gpu->submits == 0, "FRAME data is written in place" );

	byte * mapped = static_cast<byte *>( gpu->buffers.Map( handle ) );
	Check( mapped == buffer.cpu, "Map gives the mapped heap" );
	for ( int i = 0; i < 64; i++ ) {
		mapped[i] = data[i] = static_cast<byte>( 0xA0 + i );
	}
	gpu->buffers.Unmap( handle, 0, 64 );
	Check( gpu->buffers.InPlaceBytesThisFrame() == 64 && gpu->uploader.BytesThisFrame() == 0, "Unmap counts, copies nothing" );
	const std::vector<byte> read = gpu->Read( *buffer.resource.Get(), nullptr, 0, BYTES );
	Check( read.size() == BYTES && std::equal( data.begin(), data.end(), read.begin() ), "the GPU reads what the CPU wrote" );
	Check( read.size() == BYTES && std::all_of( read.begin() + 1024, read.end(), []( byte b ) { return b == 0; } ),
		"the tail is zero: committed memory starts cleared" );
	gpu->buffers.Destroy( handle );
	Check( gpu->buffers.Bytes( bufferUsage_t::FRAME ) == 0, "Destroy" );
	CheckNoLayerErrors( *gpu );
}

// The biggest texture levels do not fit a ring piece: a temporary buffer that dies with the list.
void Test_WarpLargeStage() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 );
	if ( !gpu ) {
		return;
	}
	constexpr uint64 SIZE = 20 << 20;
	const std::optional<d3d12UploadSpan_t> span = gpu->uploader.Stage( SIZE, 512 );
	if ( !Check( span.has_value(), "Stage( 20 MB )" ) ) {
		return;
	}
	Check( gpu->graveyard.Pending() == 1 && gpu->staging.Used() == 0, "a temporary buffer, buried at once, not the ring" );
	const std::vector<byte> data = Pattern( SIZE, 3 );
	memcpy( span->cpu, data.data(), data.size() );

	idComPtr<ID3D12Resource> target = D3D12_CreateBuffer( gpu->device, D3D12_HEAP_TYPE_DEFAULT, SIZE, D3D12_RESOURCE_STATE_COMMON,
		L"test large stage target" );
	d3d12ResourceState_t state;
	state.decays = true;
	gpu->barriers.Transition( *target.Get(), state, D3D12_RESOURCE_STATE_COPY_DEST );
	gpu->barriers.Flush();
	gpu->List().CopyBufferRegion( target.Get(), 0, span->buffer, span->offset, SIZE );
	gpu->WaitIdle();
	Check( gpu->graveyard.Pending() == 0, "collected after the wait for its list" );
	Check( gpu->Read( *target.Get(), &state, 0, SIZE ) == data, "the copy equals the data" );
	CheckNoLayerErrors( *gpu );
}

// R3: a destroyed buffer dies after an explicit wait for its list, never because the GPU happens to be done.
void Test_WarpGravesWaitForWaited() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 );
	if ( !gpu ) {
		return;
	}
	const std::vector<byte> data = Pattern( 4096, 4 );
	const bufferHandle_t handle = gpu->buffers.Create( bufferType_t::INDEX, bufferUsage_t::STATIC, 4096, data.data() );
	gpu->buffers.Destroy( handle );
	Check( gpu->graveyard.Pending() == 1, "buried with the list that copies into it" );
	const uint64 fence = gpu->SubmitChunk();
	gpu->FinishUnwaited();
	gpu->graveyard.Collect();
	Check( gpu->graveyard.Pending() == 1 && gpu->queue.Waited() < fence, "the GPU is done, nobody waited: it stays" );
	gpu->WaitFor( fence );
	Check( gpu->graveyard.Pending() == 0, "the wait frees it" );
	CheckNoLayerErrors( *gpu );
}

void Test_WarpRingRetiresByWaited() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 );
	if ( !gpu ) {
		return;
	}
	Check( gpu->staging.TryAllocate( 16 << 20, 16 ).has_value(), "the whole ring" );
	static_cast<void>( gpu->List() );
	const uint64 fence = gpu->SubmitChunk();
	gpu->FinishUnwaited();
	Check( !gpu->staging.TryAllocate( 16, 16 ) && gpu->staging.OldestFence() == fence, "the GPU is done, nobody waited: still full" );
	gpu->WaitFor( fence );
	Check( gpu->staging.TryAllocate( 16, 16 ).has_value(), "the wait frees it" );
	CheckNoLayerErrors( *gpu );
}

struct testCase_t {
	const char *	name;
	void			( *run )();
};

constexpr testCase_t TESTS[] = {
	{ "Ring_Wrap", Test_RingWrap },
	{ "Ring_Release", Test_RingRelease },
	{ "Ring_Full", Test_RingFull },
	{ "Handle_Generations", Test_HandleGenerations },
	{ "Handle_FindDead", Test_HandleFindDead },
	{ "Handle_LowestFree", Test_HandleLowestFree },
	{ "Handle_Clear", Test_HandleClear },
	{ "Handle_Full", Test_HandleFull },
	{ "Handle_FindDuringAdd", Test_HandleFindDuringAdd },
	{ "Warp_StaticBuffer", Test_WarpStaticBuffer },
	{ "Warp_FrameBuffers", Test_WarpFrameBuffers },
	{ "Warp_LargeStage", Test_WarpLargeStage },
	{ "Warp_GravesWaitForWaited", Test_WarpGravesWaitForWaited },
	{ "Warp_RingRetiresByWaited", Test_WarpRingRetiresByWaited },
};

}	// namespace

int main( int argc, char ** argv ) {
	const char * filter = ( argc > 1 && idStr::Icmp( argv[1], "all" ) != 0 ) ? argv[1] : nullptr;
	if ( getenv( "D3D12_TESTS_VERBOSE" ) != nullptr ) {
		maxFailuresPerTest = 1 << 30;
	}
	idLib::Init();
	printf( "d3d12 tests\n" );

	int numRun = 0;
	int numFailedTests = 0;
	for ( const testCase_t & test : TESTS ) {
		if ( filter != nullptr && strstr( test.name, filter ) == nullptr ) {
			continue;
		}
		numFailuresInTest = 0;
		printf( "[ RUN  ] %s\n", test.name );
		fflush( stdout );
		test.run();
		if ( numFailuresInTest > maxFailuresPerTest ) {
			printf( "  ... %d more failures\n", numFailuresInTest - maxFailuresPerTest );
		}
		printf( "[ %s ] %s\n", ( numFailuresInTest == 0 ) ? " OK " : "FAIL", test.name );
		numRun++;
		numFailedTests += ( numFailuresInTest != 0 ) ? 1 : 0;
	}

	printf( "\n%d tests, %d checks, %d failed checks, %d failed tests\n", numRun, numChecks, numFailures, numFailedTests );
	return ( numFailedTests == 0 ) ? 0 : 1;
}
