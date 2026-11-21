// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../idlib/precompiled.h"

#include "../renderer/tr_local.h"
#include "../renderer/D3D12/d3d12_Buffers.h"
#include "../renderer/D3D12/d3d12_Copies.h"
#include "../renderer/D3D12/d3d12_Draw.h"
#include "../renderer/D3D12/d3d12_Hdr.h"
#include "../renderer/D3D12/d3d12_Dxc.h"
#include "../renderer/D3D12/d3d12_HlslCompiler.h"
#include "../renderer/D3D12/d3d12_Pipelines.h"
#include "../renderer/D3D12/d3d12_Programs.h"
#include "../renderer/D3D12/d3d12_RenderGraph.h"
#include "../renderer/D3D12/d3d12_StateMap.h"
#include "../renderer/D3D12/d3d12_Textures.h"
#include "../renderer/D3D12/d3d12_Timers.h"
#include "../renderer/DXT/DXTCodec.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/*
	d3d12Tests: the parts of the Direct3D 12 backend that run without a window. The ring, the handle
	table and the format and sampler tables are pure arithmetic; the uploader, the buffer and texture
	stores and the sampler heap run on a WARP device with the debug layer. The texture tests that a
	driver could get wrong (Q3's border, RGB565, BC decoding) also run on the GPU, when there is one.
	DXC compiles every stage of the repo's base/shaders, and the reflection of every program is held
	to what GL reported on mars_city1 (neo/tests/d3d12_golden/renderProgInfo-gl.txt).
	The pipeline state map runs per GLS family and on random keys; the PSO factory, the journal replayed
	into the next run, the pipeline library and the render graph's compute and raster passes on WARP. The screen
	copies run on a 64 x 64 scene target with one and four samples, on WARP and the GPU, and so do the
	GPU timers of r_pix and the draw recorder over every placement of the vertex cache's FRAME buffers.

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
	idTestGpu( idD3D12Library && loadedLibrary, idD3D12Device && createdDevice, uint64 stagingSize, d3d12BufferPlacement_t framePlacement ) :
		library( std::move( loadedLibrary ) ),
		device( std::move( createdDevice ) ),
		liveObjects( device ),
		queue( device ),
		graveyard( queue ),
		commands( device, queue ),
		barriers( *this ),
		srvPool( device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SHADER_VISIBLE_SRVS ),
		rtvPool( device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV ),
		samplers( device, graveyard ),
		staging( device, stagingSize, L"test staging ring" ),
		uploader( device, *this, staging, barriers, graveyard ),
		buffers( device, graveyard, uploader, barriers, framePlacement ),
		textures( device, graveyard, srvPool, rtvPool, samplers, uploader, barriers, device.Features().unalignedBlocks ) {
	}
	~idTestGpu() {
		if ( !device.IsLost() ) {
			WaitIdle();
		}
		buffers.DestroyAll();
		textures.DestroyAll();
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
	static constexpr uint32			SHADER_VISIBLE_SRVS = 4096;
	idD3D12DescriptorPool			srvPool;
	idD3D12DescriptorPool			rtvPool;
	idD3D12SamplerHeap				samplers;
	idD3D12UploadRing				staging;
	idD3D12Uploader					uploader;
	idD3D12BufferStore				buffers;
	idD3D12TextureStore				textures;
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

[[nodiscard]] std::unique_ptr<idTestGpu> CreateWarp( uint64 stagingSize, d3d12BufferPlacement_t framePlacement = d3d12BufferPlacement_t::UPLOAD ) {
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
		printf( "  no debug layer (D3D12\\d3d12SDKLayers.dll): the API usage is not validated\n" );
	}
	return std::make_unique<idTestGpu>( std::move( *library ), std::move( *device ), stagingSize, framePlacement );
}

// The high-performance adapter; nullptr (the test skipped, not failed) on a machine without one.
[[nodiscard]] std::unique_ptr<idTestGpu> CreateHardware( uint64 stagingSize, d3d12BufferPlacement_t framePlacement = d3d12BufferPlacement_t::UPLOAD ) {
	auto library = idD3D12Library::Load();
	if ( !library ) {
		Check( false, va( "idD3D12Library::Load: %s", library.error().c_str() ) );
		return nullptr;
	}
	auto device = idD3D12Device::Create( *library, "", 1 );
	if ( !device || device->Features().warp ) {
		printf( "  skipped: no hardware adapter\n" );
		return nullptr;
	}
	printf( "  adapter %s\n", device->Features().adapterName.c_str() );
	return std::make_unique<idTestGpu>( std::move( *library ), std::move( *device ), stagingSize, framePlacement );
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

// A FRAME buffer in each place r_d3d12FrameBuffers has: what the CPU writes between Map and Unmap reaches the GPU;
// only a STAGED one goes through the staging ring.
void FrameBuffers( idTestGpu & gpu, d3d12BufferPlacement_t placement ) {
	gpu.buffers.CreateDefaults();
	Check( gpu.buffers.ZeroJoints() != 0, "the zero joints" );
	const D3D12_VERTEX_BUFFER_VIEW attribs = gpu.buffers.DefaultAttribs();
	Check( attribs.BufferLocation != 0 && attribs.SizeInBytes == 16 && attribs.StrideInBytes == 0, "the default attribute: stride 0" );

	std::vector<byte> data = Pattern( 1024, 2 );
	const bufferHandle_t handle = gpu.buffers.Create( bufferType_t::JOINT, bufferUsage_t::FRAME, 1024, data.data() );
	if ( !Check( handle != bufferHandle_t::NONE, "Create" ) ) {
		return;
	}
	const d3d12Buffer_t & buffer = gpu.buffers.Get( handle );
	constexpr uint64 BYTES = 1024 + idD3D12BufferStore::JOINT_TAIL;
	const bool staged = ( placement == d3d12BufferPlacement_t::STAGED );
	Check( buffer.placement == placement && buffer.bytes == BYTES, va( "FRAME in %s, with the joint tail", D3D12_PlacementName( placement ) ) );
	Check( gpu.buffers.Bytes( bufferUsage_t::FRAME ) == BYTES && gpu.buffers.MirrorBytes() == ( staged ? 1024u : 0u ), "the byte counts" );
	Check( staged ? gpu.uploader.BytesThisFrame() == 1024 : ( gpu.uploader.BytesThisFrame() == 0 && gpu.submits == 0 ),
		"the data is copied only when staged" );

	byte * mapped = static_cast<byte *>( gpu.buffers.Map( handle ) );
	Check( mapped == buffer.cpu, "Map gives the mapped heap or the mirror" );
	for ( int i = 0; i < 64; i++ ) {
		mapped[i] = data[i] = static_cast<byte>( 0xA0 + i );
	}
	gpu.uploader.ResetFrameCounters();
	gpu.buffers.Unmap( handle, 0, 64 );
	Check( staged ? ( gpu.buffers.InPlaceBytesThisFrame() == 0 && gpu.uploader.BytesThisFrame() == 64 )
		: ( gpu.buffers.InPlaceBytesThisFrame() == 64 && gpu.uploader.BytesThisFrame() == 0 ), "Unmap copies the dirty range of a staged buffer only" );
	gpu.WaitIdle();		// a staged buffer decays to COMMON
	d3d12ResourceState_t decayed;
	decayed.decays = true;
	const std::vector<byte> read = gpu.Read( *buffer.resource.Get(), staged ? &decayed : nullptr, 0, BYTES );
	Check( read.size() == BYTES && std::equal( data.begin(), data.end(), read.begin() ), "the GPU reads what the CPU wrote" );
	Check( read.size() == BYTES && std::all_of( read.begin() + 1024, read.end(), []( byte b ) { return b == 0; } ),
		"the tail is zero: committed memory starts cleared" );
	gpu.buffers.Destroy( handle );
	Check( gpu.buffers.Bytes( bufferUsage_t::FRAME ) == 0 && gpu.buffers.MirrorBytes() == 0, "Destroy" );
	CheckNoLayerErrors( gpu );
}

void Test_WarpFrameBuffers() {
	if ( const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 ) ) {
		FrameBuffers( *gpu, d3d12BufferPlacement_t::UPLOAD );
	}
}

void Test_WarpFrameBuffersStaged() {
	if ( const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20, d3d12BufferPlacement_t::STAGED ) ) {
		FrameBuffers( *gpu, d3d12BufferPlacement_t::STAGED );
	}
}

// OPTIONS16: Resizable BAR; the context falls back to UPLOAD without it.
void Test_GpuFrameBuffersGpuUpload() {
	const std::unique_ptr<idTestGpu> gpu = CreateHardware( 16 << 20, d3d12BufferPlacement_t::GPU_UPLOAD );
	if ( !gpu ) {
		return;
	}
	if ( !gpu->device.Features().gpuUploadHeap ) {
		printf( "  skipped: the adapter has no GPU upload heap\n" );
		return;
	}
	FrameBuffers( *gpu, d3d12BufferPlacement_t::GPU_UPLOAD );
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

// ---- the format table and the sampler map --------------------------------------------------------------------

enum testMap_t : UINT { R = 0, G = 1, B = 2, A = 3, ZERO = 4, ONE = 5 };

[[nodiscard]] constexpr UINT Mapping( testMap_t r, testMap_t g, testMap_t b, testMap_t a ) {
	return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING( r, g, b, a );
}

struct formatRow_t {
	const char *		name;
	textureFormat_t		format;
	textureColor_t		color;
	textureRole_t		role;
	int					width, height;
	bool				unalignedBlocks;
	DXGI_FORMAT			resource;
	DXGI_FORMAT			rtv;
	UINT				mapping;
	uint8				blockBytes, sourceBytes;
	bool				compressed, sourceBlocks;
	d3d12Convert_t		convert;
};

void Test_FormatTable() {
	using enum d3d12Convert_t;
	constexpr textureRole_t SAMPLED = textureRole_t::SAMPLED;
	const UINT IDENTITY = Mapping( R, G, B, A );
	const formatRow_t rows[] = {
		{ "RGBA8", FMT_RGBA8, CFM_DEFAULT, SAMPLED, 64, 64, true, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, IDENTITY, 4, 4, false, false, NONE },
		{ "COLOR_COPY", FMT_DXT5, CFM_DEFAULT, textureRole_t::COLOR_COPY, 64, 64, true, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, IDENTITY, 4, 4,
			false, false, NONE },
		{ "DEPTH_COPY wins over RGBA8", FMT_RGBA8, CFM_DEFAULT, textureRole_t::DEPTH_COPY, 64, 64, true, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
			Mapping( R, R, R, ONE ), 4, 4, false, false, NONE },
		{ "XRGB8", FMT_XRGB8, CFM_DEFAULT, SAMPLED, 64, 64, true, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, Mapping( R, G, B, ONE ), 4, 4,
			false, false, NONE },
		{ "ALPHA", FMT_ALPHA, CFM_DEFAULT, SAMPLED, 64, 64, true, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN, Mapping( ONE, ONE, ONE, R ), 1, 1,
			false, false, NONE },
		{ "L8A8", FMT_L8A8, CFM_DEFAULT, SAMPLED, 64, 64, true, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN, Mapping( R, R, R, G ), 2, 2,
			false, false, NONE },
		{ "LUM8", FMT_LUM8, CFM_DEFAULT, SAMPLED, 64, 64, true, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN, Mapping( R, R, R, ONE ), 1, 1,
			false, false, NONE },
		{ "INT8 (Q3: R8, the mapping applies to the border)", FMT_INT8, CFM_DEFAULT, SAMPLED, 64, 64, true, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN,
			Mapping( R, R, R, R ), 1, 1, false, false, NONE },
		{ "DXT1", FMT_DXT1, CFM_DEFAULT, SAMPLED, 64, 64, false, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_UNKNOWN, IDENTITY, 8, 8, true, true, NONE },
		{ "DXT1 green alpha", FMT_DXT1, CFM_GREEN_ALPHA, SAMPLED, 64, 64, false, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_UNKNOWN, Mapping( ONE, ONE, ONE, G ),
			8, 8, true, true, NONE },
		{ "DXT5", FMT_DXT5, CFM_NORMAL_DXT5, SAMPLED, 64, 64, false, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_UNKNOWN, IDENTITY, 16, 16, true, true, NONE },
		{ "DXT1 6x6 with OPTIONS8", FMT_DXT1, CFM_DEFAULT, SAMPLED, 6, 6, true, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_UNKNOWN, IDENTITY, 8, 8,
			true, true, NONE },
		{ "DXT1 6x8 without OPTIONS8", FMT_DXT1, CFM_DEFAULT, SAMPLED, 6, 8, false, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, IDENTITY, 4, 8,
			false, true, DECODE_DXT1 },
		{ "DXT5 8x2 without OPTIONS8", FMT_DXT5, CFM_DEFAULT, SAMPLED, 8, 2, false, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, IDENTITY, 4, 16,
			false, true, DECODE_DXT5 },
		{ "DXT1 decoded, green alpha", FMT_DXT1, CFM_GREEN_ALPHA, SAMPLED, 2, 2, false, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
			Mapping( ONE, ONE, ONE, G ), 4, 8, false, true, DECODE_DXT1 },
		{ "RGB565", FMT_RGB565, CFM_DEFAULT, SAMPLED, 64, 64, true, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_UNKNOWN, Mapping( R, G, B, ONE ), 2, 2,
			false, false, SWAP_565 },
		{ "DEPTH", FMT_DEPTH, CFM_DEFAULT, SAMPLED, 64, 64, true, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, Mapping( R, R, R, ONE ), 4, 4,
			false, false, NONE },
		{ "X16", FMT_X16, CFM_DEFAULT, SAMPLED, 64, 64, true, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN, Mapping( R, R, R, R ), 2, 2, false, false, NONE },
		{ "Y16_X16", FMT_Y16_X16, CFM_DEFAULT, SAMPLED, 64, 64, true, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_UNKNOWN, Mapping( R, R, R, G ), 4, 4,
			false, false, NONE },
		{ "NONE", FMT_NONE, CFM_DEFAULT, SAMPLED, 64, 64, true, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0, 0,
			false, false, NONE },
	};
	for ( const formatRow_t & row : rows ) {
		const d3d12FormatInfo_t info = D3D12_FormatFor( row.format, row.color, row.role, row.width, row.height, row.unalignedBlocks );
		Check( info.resource == row.resource && info.srv == row.resource, va( "%s: resource and view format %d", row.name, info.resource ) );
		Check( info.rtv == row.rtv, va( "%s: render target view", row.name ) );
		Check( info.mapping == row.mapping, va( "%s: mapping 0x%x, expected 0x%x", row.name, info.mapping, row.mapping ) );
		Check( info.blockBytes == row.blockBytes && info.sourceBytes == row.sourceBytes, va( "%s: bytes %d / %d", row.name, info.blockBytes,
			info.sourceBytes ) );
		Check( info.compressed == row.compressed && info.sourceBlocks == row.sourceBlocks, va( "%s: blocks", row.name ) );
		Check( info.convert == row.convert, va( "%s: conversion %d", row.name, static_cast<int>( info.convert ) ) );
	}
}

void Test_SamplerPacking() {
	constexpr samplerDesc_t DEFAULT_SAMPLER = { samplerFilter_t::NEAREST, samplerAddress_t::REPEAT, 1.0f, 0.0f };
	Check( D3D12_PackSampler( DEFAULT_SAMPLER ) == 0, "point, repeat, no anisotropy, no bias packs to 0" );
	Check( D3D12_UnpackSampler( 0 ) == DEFAULT_SAMPLER, "0 unpacks to it" );
	const samplerDesc_t samplers[] = {
		{ samplerFilter_t::LINEAR_MIP_LINEAR, samplerAddress_t::BORDER_TRANSPARENT, 16.0f, -0.5f },
		{ samplerFilter_t::LINEAR_MIP_NEAREST, samplerAddress_t::CLAMP, 8.0f, 0.25f },
		{ samplerFilter_t::LINEAR, samplerAddress_t::BORDER_BLACK, 1.0f, 3.0f },
	};
	for ( const samplerDesc_t & sampler : samplers ) {
		Check( D3D12_UnpackSampler( D3D12_PackSampler( sampler ) ) == sampler, va( "filter %d address %d: the round trip",
			static_cast<int>( sampler.filter ), static_cast<int>( sampler.address ) ) );
	}
	Check( D3D12_PackSampler( samplers[0] ) != D3D12_PackSampler( { samplerFilter_t::LINEAR_MIP_LINEAR, samplerAddress_t::BORDER_TRANSPARENT, 16.0f, 0.5f } ),
		"the bias is part of the key" );
	Check( D3D12_UnpackSampler( D3D12_PackSampler( { samplerFilter_t::LINEAR, samplerAddress_t::REPEAT, 99.0f, 0.0f } ) ).maxAnisotropy == 16.0f,
		"anisotropy is clamped to 16" );
}

void Test_SamplerMap() {
	struct row_t {
		samplerFilter_t		filter;
		float				anisotropy;
		bool				anisoPointMip;
		D3D12_FILTER		expected;
		float				maxLod;
	};
	const row_t rows[] = {
		{ samplerFilter_t::NEAREST, 1.0f, false, D3D12_FILTER_MIN_MAG_MIP_POINT, 0.0f },
		{ samplerFilter_t::LINEAR, 1.0f, false, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, 0.0f },
		{ samplerFilter_t::LINEAR_MIP_NEAREST, 1.0f, true, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_FLOAT32_MAX },
		{ samplerFilter_t::LINEAR_MIP_NEAREST, 8.0f, true, D3D12_FILTER_MIN_MAG_ANISOTROPIC_MIP_POINT, D3D12_FLOAT32_MAX },
		{ samplerFilter_t::LINEAR_MIP_NEAREST, 8.0f, false, D3D12_FILTER_ANISOTROPIC, D3D12_FLOAT32_MAX },
		{ samplerFilter_t::LINEAR_MIP_LINEAR, 1.0f, true, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_FLOAT32_MAX },
		{ samplerFilter_t::LINEAR_MIP_LINEAR, 4.0f, true, D3D12_FILTER_ANISOTROPIC, D3D12_FLOAT32_MAX },
		{ samplerFilter_t::LINEAR_MIP_LINEAR, 4.0f, false, D3D12_FILTER_ANISOTROPIC, D3D12_FLOAT32_MAX },
	};
	for ( const row_t & row : rows ) {
		const D3D12_SAMPLER_DESC desc = D3D12_SamplerFor( D3D12_PackSampler( { row.filter, samplerAddress_t::REPEAT, row.anisotropy, 0.0f } ),
			row.anisoPointMip );
		Check( desc.Filter == row.expected, va( "filter %d, anisotropy %g, OPTIONS19 %d: D3D12 filter 0x%x", static_cast<int>( row.filter ),
			row.anisotropy, row.anisoPointMip, desc.Filter ) );
		Check( desc.MaxLOD == row.maxLod && desc.MinLOD == 0.0f, va( "filter %d: MaxLOD %g", static_cast<int>( row.filter ), desc.MaxLOD ) );
		Check( desc.MaxAnisotropy == static_cast<UINT>( row.anisotropy ), "MaxAnisotropy" );
	}
	struct address_t {
		samplerAddress_t			address;
		D3D12_TEXTURE_ADDRESS_MODE	mode;
		float						alpha;
	};
	const address_t addresses[] = {
		{ samplerAddress_t::REPEAT, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0.0f },
		{ samplerAddress_t::CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f },
		{ samplerAddress_t::BORDER_BLACK, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 1.0f },
		{ samplerAddress_t::BORDER_TRANSPARENT, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0.0f },
	};
	for ( const address_t & row : addresses ) {
		const D3D12_SAMPLER_DESC desc = D3D12_SamplerFor( D3D12_PackSampler( { samplerFilter_t::LINEAR, row.address, 1.0f, 0.0f } ), false );
		Check( desc.AddressU == row.mode && desc.AddressV == row.mode && desc.AddressW == row.mode, va( "address %d", static_cast<int>( row.address ) ) );
		Check( desc.BorderColor[0] == 0.0f && desc.BorderColor[1] == 0.0f && desc.BorderColor[2] == 0.0f && desc.BorderColor[3] == row.alpha,
			va( "address %d: border colour", static_cast<int>( row.address ) ) );
	}
	const D3D12_SAMPLER_DESC biased = D3D12_SamplerFor( D3D12_PackSampler( { samplerFilter_t::LINEAR, samplerAddress_t::REPEAT, 1.0f, 40.0f } ), false );
	Check( biased.MipLODBias == D3D12_MIP_LOD_BIAS_MAX, "the bias is clamped to D3D12's range" );
	Check( D3D12_SamplerFor( D3D12_PackSampler( { samplerFilter_t::LINEAR, samplerAddress_t::REPEAT, 1.0f, -0.5f } ), false ).MipLODBias == -0.5f,
		"the bias" );
}

// A slot per packed sampler that something holds, the fallback's first; the views of an SRV pool reach its
// shader-visible heap at their slots.
void Test_WarpDescriptorSlots() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 );
	if ( !gpu ) {
		return;
	}
	{
		idD3D12SamplerHeap heap( gpu->device, gpu->graveyard );
		const uint64 linear = D3D12_PackSampler( { samplerFilter_t::LINEAR, samplerAddress_t::REPEAT, 1.0f, 0.0f } );
		const uint64 point = D3D12_PackSampler( { samplerFilter_t::NEAREST, samplerAddress_t::CLAMP, 1.0f, 0.0f } );
		heap.ResetFrameCounters();
		const uint32 first = heap.Acquire( linear );
		const uint32 second = heap.Acquire( point );
		Check( heap.FallbackSlot() == 0 && first == 1 && second == 2 && heap.Acquire( linear ) == 1 && heap.Used() == 3 && heap.BuiltThisFrame() == 2,
			"the fallback and two samplers, three slots, each made once" );
		heap.ResetFrameCounters();
		Check( heap.Acquire( point ) == 2 && heap.BuiltThisFrame() == 0, "a held sampler is not made again" );
		Check( heap.Heap() != nullptr && heap.Heap()->GetDesc().NumDescriptors == idD3D12SamplerHeap::SIZE
			&& ( heap.Heap()->GetDesc().Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE ) != 0, "one shader-visible heap of 2048" );
	}

	idD3D12DescriptorPool pool( gpu->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8 );
	const d3d12Descriptor_t slot = pool.Allocate();
	Check( slot.index == 0 && pool.Used() == 1 && pool.ShaderVisibleHeap() != nullptr && pool.ShaderVisibleHeap()->GetDesc().NumDescriptors == 8,
		"the pool's shader-visible heap" );
	CheckNoLayerErrors( *gpu );
}

// ---- textures, sampled as a shader sees them -----------------------------------------------------------------

constexpr std::string_view SAMPLE_HLSL = R"(
cbuffer points : register( b0 ) {
	float4 coords[16];		// xy: uv, or xyz: a direction; w: the mip level
};
Texture2D<float4> image : register( t0 );
TextureCube<float4> cube : register( t1 );
SamplerState imageSampler : register( s0 );
float4 vsMain( uint vertexId : SV_VertexID ) : SV_Position {
	float2 corner = float2( ( vertexId << 1 ) & 2, vertexId & 2 );
	return float4( corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0 );
}
float4 psImage( float4 position : SV_Position ) : SV_Target {
	float4 c = coords[uint( position.x )];
	return image.SampleLevel( imageSampler, c.xy, c.w );
}
float4 psCube( float4 position : SV_Position ) : SV_Target {
	float4 c = coords[uint( position.x )];
	return cube.SampleLevel( imageSampler, c.xyz, c.w );
}
)";

// Draws one pixel per point into a float target and reads them back.
class idTestSampler {
public:
	static constexpr int			POINTS = 16;
	explicit						idTestSampler( idTestGpu & gpu );
	[[nodiscard]] bool				Ready() const { return pipelines[0] && pipelines[1] && mapped != nullptr; }
	// The view must be in PIXEL_SHADER_RESOURCE once the batch is flushed.
	[[nodiscard]] std::vector<idVec4>	Sample( d3d12Descriptor_t view, textureKind_t kind, const D3D12_SAMPLER_DESC & sampler,
										std::span<const idVec4> points );

private:
	[[nodiscard]] d3d12Bytecode_t	Compile( const char * entry, const char * profile );

	idTestGpu &						gpu;
	idComPtr<ID3D12RootSignature>	root;
	std::array<idComPtr<ID3D12PipelineState>, 2>	pipelines;	// by textureKind_t
	idComPtr<ID3D12DescriptorHeap>	viewHeap, samplerHeap, rtvHeap;
	idComPtr<ID3D12Resource>		target, constants, readback;
	d3d12ResourceState_t			targetState;
	idVec4 *						mapped = nullptr;
};

d3d12Bytecode_t idTestSampler::Compile( const char * entry, const char * profile ) {
	auto output = D3D12_DxcCompile( gpu.library, SAMPLE_HLSL, "d3d12 test sampler", entry, profile );
	Check( output.has_value(), va( "compile %s: %s", entry, output ? "" : output.error().c_str() ) );
	return output ? std::move( output->code ) : d3d12Bytecode_t();
}

idTestSampler::idTestSampler( idTestGpu & owner ) : gpu( owner ) {
	ID3D12Device2 & device = gpu.device.Get();
	D3D12_DESCRIPTOR_RANGE1 views = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0 };
	D3D12_DESCRIPTOR_RANGE1 samplers = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0 };
	std::array<D3D12_ROOT_PARAMETER1, 3> parameters = {};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[1].DescriptorTable = { 1, &views };
	parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[2].DescriptorTable = { 1, &samplers };
	parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
	desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = static_cast<UINT>( parameters.size() );
	desc.Desc_1_1.pParameters = parameters.data();
	idComPtr<ID3DBlob> blob;
	idComPtr<ID3DBlob> error;
	if ( !Check( SUCCEEDED( gpu.library.serializeRootSignature( &desc, &blob, &error ) ), "serialize the test root signature" )
		|| !gpu.device.Check( device.CreateRootSignature( 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS( &root ) ),
			"CreateRootSignature (test)" ) ) {
		return;
	}

	const d3d12Bytecode_t vertex = Compile( "vsMain", "vs_6_6" );
	for ( const textureKind_t kind : { textureKind_t::TEX_2D, textureKind_t::TEX_CUBE } ) {
		const d3d12Bytecode_t pixel = Compile( ( kind == textureKind_t::TEX_2D ) ? "psImage" : "psCube", "ps_6_6" );
		if ( vertex.empty() || pixel.empty() ) {
			return;
		}
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
		pso.pRootSignature = root.Get();
		pso.VS = { vertex.data(), vertex.size() };
		pso.PS = { pixel.data(), pixel.size() };
		pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pso.SampleMask = UINT_MAX;
		pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pso.NumRenderTargets = 1;
		pso.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
		pso.SampleDesc.Count = 1;
		gpu.device.Check( device.CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( &pipelines[static_cast<size_t>( kind )] ) ), "CreateGraphicsPipelineState (test)" );
	}

	const auto heap = [&]( D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool visible ) {
		const D3D12_DESCRIPTOR_HEAP_DESC heapDesc = { type, count, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
		idComPtr<ID3D12DescriptorHeap> created;
		gpu.device.Check( device.CreateDescriptorHeap( &heapDesc, IID_PPV_ARGS( &created ) ), "CreateDescriptorHeap (test)" );
		return created;
	};
	viewHeap = heap( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true );
	samplerHeap = heap( D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, true );
	rtvHeap = heap( D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false );

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC targetDesc = {};
	targetDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	targetDesc.Width = POINTS;
	targetDesc.Height = 1;
	targetDesc.DepthOrArraySize = 1;
	targetDesc.MipLevels = 1;
	targetDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	targetDesc.SampleDesc.Count = 1;
	targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if ( !gpu.device.Check( device.CreateCommittedResource( &defaultHeap, D3D12_HEAP_FLAG_NONE, &targetDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
			IID_PPV_ARGS( &target ) ), "CreateCommittedResource (test target)" ) ) {
		return;
	}
	targetState.current = D3D12_RESOURCE_STATE_RENDER_TARGET;
	device.CreateRenderTargetView( target.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart() );
	constants = D3D12_CreateBuffer( gpu.device, D3D12_HEAP_TYPE_UPLOAD, POINTS * sizeof( idVec4 ), D3D12_RESOURCE_STATE_GENERIC_READ, L"test points" );
	readback = D3D12_CreateBuffer( gpu.device, D3D12_HEAP_TYPE_READBACK, POINTS * sizeof( idVec4 ), D3D12_RESOURCE_STATE_COPY_DEST, L"test sampled" );
	const D3D12_RANGE nothingRead = { 0, 0 };
	void * cpu = nullptr;
	if ( constants && readback && gpu.device.Check( constants->Map( 0, &nothingRead, &cpu ), "Map (test points)" ) ) {
		mapped = static_cast<idVec4 *>( cpu );
	}
}

std::vector<idVec4> idTestSampler::Sample( d3d12Descriptor_t view, textureKind_t kind, const D3D12_SAMPLER_DESC & sampler, std::span<const idVec4> points ) {
	if ( !Ready() || points.size() > POINTS ) {
		Check( false, "the test sampler is not ready" );
		return {};
	}
	ID3D12Device2 & device = gpu.device.Get();
	const UINT increment = device.GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
	const UINT slot = ( kind == textureKind_t::TEX_CUBE ) ? 1 : 0;
	D3D12_CPU_DESCRIPTOR_HANDLE views = viewHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_SHADER_RESOURCE_VIEW_DESC none = {};
	none.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	none.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	none.ViewDimension = ( slot == 0 ) ? D3D12_SRV_DIMENSION_TEXTURECUBE : D3D12_SRV_DIMENSION_TEXTURE2D;
	none.Texture2D.MipLevels = 1;
	D3D12_CPU_DESCRIPTOR_HANDLE other = views;
	other.ptr += static_cast<SIZE_T>( 1 - slot ) * increment;
	device.CreateShaderResourceView( nullptr, &none, other );
	D3D12_CPU_DESCRIPTOR_HANDLE mine = views;
	mine.ptr += static_cast<SIZE_T>( slot ) * increment;
	device.CopyDescriptorsSimple( 1, mine, gpu.srvPool.Cpu( view ), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
	device.CreateSampler( &sampler, samplerHeap->GetCPUDescriptorHandleForHeapStart() );
	std::fill_n( mapped, POINTS, idVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
	std::copy( points.begin(), points.end(), mapped );

	gpu.barriers.Transition( *target.Get(), targetState, D3D12_RESOURCE_STATE_RENDER_TARGET );
	gpu.barriers.Flush();
	ID3D12GraphicsCommandList1 & list = gpu.List();
	ID3D12DescriptorHeap * heaps[] = { viewHeap.Get(), samplerHeap.Get() };
	list.SetDescriptorHeaps( 2, heaps );
	list.SetGraphicsRootSignature( root.Get() );
	list.SetPipelineState( pipelines[slot].Get() );
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	list.OMSetRenderTargets( 1, &rtv, FALSE, nullptr );
	const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>( POINTS ), 1.0f, 0.0f, 1.0f };
	const D3D12_RECT scissor = { 0, 0, POINTS, 1 };
	list.RSSetViewports( 1, &viewport );
	list.RSSetScissorRects( 1, &scissor );
	list.IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	list.SetGraphicsRootConstantBufferView( 0, constants->GetGPUVirtualAddress() );
	list.SetGraphicsRootDescriptorTable( 1, viewHeap->GetGPUDescriptorHandleForHeapStart() );
	list.SetGraphicsRootDescriptorTable( 2, samplerHeap->GetGPUDescriptorHandleForHeapStart() );
	list.DrawInstanced( 3, 1, 0, 0 );

	gpu.barriers.Transition( *target.Get(), targetState, D3D12_RESOURCE_STATE_COPY_SOURCE );
	gpu.barriers.Flush();
	D3D12_TEXTURE_COPY_LOCATION destination = {};
	destination.pResource = readback.Get();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, POINTS, 1, 1, POINTS * sizeof( idVec4 ) };
	D3D12_TEXTURE_COPY_LOCATION source = {};
	source.pResource = target.Get();
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	list.CopyTextureRegion( &destination, 0, 0, 0, &source, nullptr );
	gpu.WaitIdle();

	std::vector<idVec4> result( points.size() );
	const D3D12_RANGE everything = { 0, POINTS * sizeof( idVec4 ) };
	void * read = nullptr;
	if ( gpu.device.Check( readback->Map( 0, &everything, &read ), "Map (test sampled)" ) ) {
		memcpy( result.data(), read, result.size() * sizeof( idVec4 ) );
		const D3D12_RANGE nothingWritten = { 0, 0 };
		readback->Unmap( 0, &nothingWritten );
	}
	return result;
}

[[nodiscard]] D3D12_SAMPLER_DESC SamplerOf( samplerFilter_t filter, samplerAddress_t address ) {
	return D3D12_SamplerFor( D3D12_PackSampler( { filter, address, 1.0f, 0.0f } ), false );
}

[[nodiscard]] bool Near( const idVec4 & a, const idVec4 & b, float tolerance ) {
	for ( int i = 0; i < 4; i++ ) {
		if ( !( std::fabs( a[i] - b[i] ) <= tolerance ) ) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] const char * Text( const idVec4 & v ) {
	return va( "(%.4f %.4f %.4f %.4f)", v.x, v.y, v.z, v.w );
}

[[nodiscard]] idImageOpts Opts( textureFormat_t format, int width, int height, int levels, textureType_t type = TT_2D,
	textureColor_t color = CFM_DEFAULT ) {
	idImageOpts opts;
	opts.textureType = type;
	opts.format = format;
	opts.colorFormat = color;
	opts.width = width;
	opts.height = height;
	opts.numLevels = levels;
	return opts;
}

constexpr samplerDesc_t POINT_CLAMP = { samplerFilter_t::NEAREST, samplerAddress_t::CLAMP, 1.0f, 0.0f };

// The 16 texel centres of a 4 x 4 level.
[[nodiscard]] std::vector<idVec4> TexelCentres4x4( float level = 0.0f ) {
	std::vector<idVec4> points;
	for ( int y = 0; y < 4; y++ ) {
		for ( int x = 0; x < 4; x++ ) {
			points.emplace_back( ( x + 0.5f ) / 4.0f, ( y + 0.5f ) / 4.0f, 0.0f, level );
		}
	}
	return points;
}

[[nodiscard]] float Unorm8( byte b ) {
	return b / 255.0f;
}

[[nodiscard]] float Unorm16( const byte * p ) {
	return static_cast<float>( p[0] | ( p[1] << 8 ) ) / 65535.0f;
}

// What GL samples from one texel of the uploaded data: its internal format, then the swizzle of GL_SetTexParameters.
[[nodiscard]] idVec4 GlTexel( textureFormat_t format, const byte * p ) {
	switch ( format ) {
		case FMT_RGBA8:		return idVec4( Unorm8( p[0] ), Unorm8( p[1] ), Unorm8( p[2] ), Unorm8( p[3] ) );
		case FMT_XRGB8:		return idVec4( Unorm8( p[0] ), Unorm8( p[1] ), Unorm8( p[2] ), 1.0f );
		case FMT_ALPHA:		return idVec4( 1.0f, 1.0f, 1.0f, Unorm8( p[0] ) );
		case FMT_L8A8:		return idVec4( Unorm8( p[0] ), Unorm8( p[0] ), Unorm8( p[0] ), Unorm8( p[1] ) );
		case FMT_LUM8:		return idVec4( Unorm8( p[0] ), Unorm8( p[0] ), Unorm8( p[0] ), 1.0f );
		case FMT_INT8:		return idVec4( Unorm8( p[0] ), Unorm8( p[0] ), Unorm8( p[0] ), Unorm8( p[0] ) );
		case FMT_X16:		return idVec4( Unorm16( p ), Unorm16( p ), Unorm16( p ), Unorm16( p ) );
		case FMT_Y16_X16:	return idVec4( Unorm16( p ), Unorm16( p ), Unorm16( p ), Unorm16( p + 2 ) );
		case FMT_DEPTH: {
			float depth;
			memcpy( &depth, p, sizeof( depth ) );
			return idVec4( depth, depth, depth, 1.0f );
		}
		case FMT_RGB565: {
			// UNPACK_SWAP_BYTES: the words of the file are big-endian
			const int word = ( p[0] << 8 ) | p[1];
			return idVec4( ( word >> 11 ) / 31.0f, ( ( word >> 5 ) & 63 ) / 63.0f, ( word & 31 ) / 31.0f, 1.0f );
		}
		default:			return idVec4( 0.0f, 0.0f, 0.0f, 0.0f );
	}
}

[[nodiscard]] int TexelBytes( textureFormat_t format ) {
	switch ( format ) {
		case FMT_ALPHA: case FMT_LUM8: case FMT_INT8:		return 1;
		case FMT_L8A8: case FMT_RGB565: case FMT_X16:		return 2;
		default:											return 4;
	}
}

[[nodiscard]] std::vector<byte> TexelData( textureFormat_t format, int texels, uint32 seed ) {
	std::vector<byte> data = Pattern( static_cast<size_t>( texels ) * TexelBytes( format ), seed );
	if ( format == FMT_DEPTH ) {
		for ( int i = 0; i < texels; i++ ) {
			const float depth = static_cast<float>( i ) / texels;
			memcpy( data.data() + i * 4, &depth, sizeof( depth ) );
		}
	}
	return data;
}

// Every uncompressed format: 4 x 4 texels uploaded and sampled at their centres equal GL's swizzled texels.
void TextureFormats( idTestGpu & gpu ) {
	idTestSampler sampler( gpu );
	if ( !sampler.Ready() ) {
		return;
	}
	const D3D12_SAMPLER_DESC point = SamplerOf( samplerFilter_t::NEAREST, samplerAddress_t::CLAMP );
	constexpr textureFormat_t FORMATS[] = { FMT_RGBA8, FMT_XRGB8, FMT_ALPHA, FMT_L8A8, FMT_LUM8, FMT_INT8, FMT_RGB565, FMT_DEPTH, FMT_X16, FMT_Y16_X16 };
	for ( const textureFormat_t format : FORMATS ) {
		const textureHandle_t handle = gpu.textures.Create( Opts( format, 4, 4, 1 ), textureRole_t::SAMPLED, POINT_CLAMP, "test format" );
		if ( !Check( handle != textureHandle_t::NONE, va( "Create format %d", format ) ) ) {
			continue;
		}
		const std::vector<byte> data = TexelData( format, 16, 10 + format );
		gpu.textures.Upload( handle, 0, 0, 4, 4, data.data(), 0 );
		gpu.textures.MakeReadable( handle );
		const std::vector<idVec4> points = TexelCentres4x4();
		const std::vector<idVec4> sampled = sampler.Sample( gpu.textures.SampledView( handle, textureKind_t::TEX_2D ), textureKind_t::TEX_2D, point, points );
		const int bytes = TexelBytes( format );
		const float tolerance = ( format == FMT_X16 || format == FMT_Y16_X16 || format == FMT_DEPTH ) ? 1e-5f : 0.5f / 255.0f;
		for ( size_t i = 0; i < sampled.size(); i++ ) {
			const idVec4 expected = GlTexel( format, data.data() + i * bytes );
			Check( Near( sampled[i], expected, tolerance ), va( "format %d texel %d: %s, GL %s", format, static_cast<int>( i ), Text( sampled[i] ),
				Text( expected ) ) );
		}
		gpu.textures.Destroy( handle );
	}
	CheckNoLayerErrors( gpu );
}

void Test_WarpTextureFormats() {
	if ( const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 ) ) {
		TextureFormats( *gpu );
	}
}

void Test_GpuTextureFormats() {
	if ( const std::unique_ptr<idTestGpu> gpu = CreateHardware( 16 << 20 ) ) {
		TextureFormats( *gpu );
	}
}

// Q3: GL swizzles the border colour, so outside the texture a BORDER sampler gives swizzle( base format( border ) ).
void Borders( idTestGpu & gpu ) {
	idTestSampler sampler( gpu );
	if ( !sampler.Ready() ) {
		return;
	}
	struct row_t {
		textureFormat_t		format;
		samplerAddress_t	address;
		idVec4				gl;
	};
	const row_t rows[] = {
		{ FMT_INT8, samplerAddress_t::BORDER_BLACK, idVec4( 0.0f, 0.0f, 0.0f, 0.0f ) },		// _noFalloff
		{ FMT_INT8, samplerAddress_t::BORDER_TRANSPARENT, idVec4( 0.0f, 0.0f, 0.0f, 0.0f ) },
		{ FMT_ALPHA, samplerAddress_t::BORDER_BLACK, idVec4( 1.0f, 1.0f, 1.0f, 0.0f ) },
		{ FMT_L8A8, samplerAddress_t::BORDER_BLACK, idVec4( 0.0f, 0.0f, 0.0f, 0.0f ) },
		{ FMT_LUM8, samplerAddress_t::BORDER_TRANSPARENT, idVec4( 0.0f, 0.0f, 0.0f, 1.0f ) },
		{ FMT_RGB565, samplerAddress_t::BORDER_TRANSPARENT, idVec4( 0.0f, 0.0f, 0.0f, 1.0f ) },	// lights with alphazeroclamp
		{ FMT_XRGB8, samplerAddress_t::BORDER_TRANSPARENT, idVec4( 0.0f, 0.0f, 0.0f, 1.0f ) },
		{ FMT_RGBA8, samplerAddress_t::BORDER_BLACK, idVec4( 0.0f, 0.0f, 0.0f, 1.0f ) },
		{ FMT_RGBA8, samplerAddress_t::BORDER_TRANSPARENT, idVec4( 0.0f, 0.0f, 0.0f, 0.0f ) },
	};
	for ( const row_t & row : rows ) {
		const textureHandle_t handle = gpu.textures.Create( Opts( row.format, 4, 4, 1 ), textureRole_t::SAMPLED, POINT_CLAMP, "test border" );
		const std::vector<byte> data( 16 * 4, 200 );
		gpu.textures.Upload( handle, 0, 0, 4, 4, data.data(), 0 );
		gpu.textures.MakeReadable( handle );
		const idVec4 points[] = { idVec4( -0.5f, 0.5f, 0.0f, 0.0f ), idVec4( 0.5f, 1.5f, 0.0f, 0.0f ), idVec4( 0.5f, 0.5f, 0.0f, 0.0f ) };
		const std::vector<idVec4> sampled = sampler.Sample( gpu.textures.SampledView( handle, textureKind_t::TEX_2D ), textureKind_t::TEX_2D,
			SamplerOf( samplerFilter_t::NEAREST, row.address ), points );
		if ( sampled.size() == 3 ) {
			printf( "  format %2d address %d: border %s, inside %s\n", row.format, static_cast<int>( row.address ), Text( sampled[0] ), Text( sampled[2] ) );
			Check( Near( sampled[0], row.gl, 1e-6f ) && Near( sampled[1], row.gl, 1e-6f ), va( "format %d address %d: border %s, GL %s", row.format,
				static_cast<int>( row.address ), Text( sampled[0] ), Text( row.gl ) ) );
			Check( Near( sampled[2], GlTexel( row.format, data.data() ), 0.5f / 255.0f ), va( "format %d: inside %s", row.format, Text( sampled[2] ) ) );
		}
		gpu.textures.Destroy( handle );
	}
	CheckNoLayerErrors( gpu );
}

void Test_WarpBorders() {
	if ( const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 ) ) {
		Borders( *gpu );
	}
}

void Test_GpuBorders() {
	if ( const std::unique_ptr<idTestGpu> gpu = CreateHardware( 16 << 20 ) ) {
		Borders( *gpu );
	}
}

// DXT blocks the decoder and the GPU agree on, a few LSB apart (hardware interpolates exactly).
[[nodiscard]] std::vector<byte> Decoded( textureFormat_t format, const std::vector<byte> & blocks, int width, int height ) {
	std::vector<byte> rgba( static_cast<size_t>( width ) * height * 4 );
	idDxtDecoder decoder;
	if ( format == FMT_DXT1 ) {
		decoder.DecompressImageDXT1( blocks.data(), rgba.data(), width, height );
	} else {
		decoder.DecompressImageDXT5( blocks.data(), rgba.data(), width, height );
	}
	return rgba;
}

// A 6 x 6 DXT1 and DXT5 base (whole blocks: 8 x 8 of data), native with OPTIONS8, decoded into RGBA8 without.
void UnalignedBlocks( idTestGpu & gpu ) {
	idTestSampler sampler( gpu );
	if ( !sampler.Ready() ) {
		return;
	}
	idD3D12TextureStore decoding( gpu.device, gpu.graveyard, gpu.srvPool, gpu.rtvPool, gpu.samplers, gpu.uploader, gpu.barriers, false );
	decoding.RecordUploads( true );
	gpu.textures.RecordUploads( true );
	const D3D12_SAMPLER_DESC point = SamplerOf( samplerFilter_t::NEAREST, samplerAddress_t::CLAMP );
	for ( const textureFormat_t format : { FMT_DXT1, FMT_DXT5 } ) {
		const std::vector<byte> blocks = Pattern( 4 * ( ( format == FMT_DXT1 ) ? 8 : 16 ), 20 + format );
		const std::vector<byte> rgba = Decoded( format, blocks, 8, 8 );
		for ( const bool native : { false, true } ) {
			idD3D12TextureStore & store = native ? gpu.textures : decoding;
			if ( native && !gpu.device.Features().unalignedBlocks ) {
				printf( "  no OPTIONS8: format %d 6 x 6 is decoded here too\n", format );
			}
			const textureHandle_t handle = store.Create( Opts( format, 6, 6, 1 ), textureRole_t::SAMPLED, POINT_CLAMP, "test unaligned" );
			if ( !Check( handle != textureHandle_t::NONE, "Create 6 x 6" ) ) {
				continue;
			}
			const bool decoded = store.Get( handle ).format.convert != d3d12Convert_t::NONE;
			Check( decoded == ( !native || !gpu.device.Features().unalignedBlocks ), va( "format %d native %d: decoded %d", format, native, decoded ) );
			store.Upload( handle, 0, 0, 6, 6, blocks.data(), 0 );
			store.MakeReadable( handle );
			std::vector<idVec4> points;
			for ( int i = 0; i < 16; i++ ) {
				const int x = ( i * 5 ) % 6;
				const int y = ( i * 7 + i / 6 ) % 6;
				points.emplace_back( ( x + 0.5f ) / 6.0f, ( y + 0.5f ) / 6.0f, static_cast<float>( x ), static_cast<float>( y ) );
			}
			std::vector<idVec4> lookups = points;
			for ( idVec4 & p : lookups ) {
				p.z = 0.0f;
				p.w = 0.0f;
			}
			const std::vector<idVec4> sampled = sampler.Sample( store.SampledView( handle, textureKind_t::TEX_2D ), textureKind_t::TEX_2D, point, lookups );
			// the decoded copy is the decoder's output exactly; the GPU's BC decoding may round differently
			const float tolerance = decoded ? 0.5f / 255.0f : 3.5f / 255.0f;
			for ( size_t i = 0; i < sampled.size(); i++ ) {
				const byte * texel = rgba.data() + ( static_cast<int>( points[i].w ) * 8 + static_cast<int>( points[i].z ) ) * 4;
				const idVec4 expected( Unorm8( texel[0] ), Unorm8( texel[1] ), Unorm8( texel[2] ), Unorm8( texel[3] ) );
				Check( Near( sampled[i], expected, tolerance ), va( "format %d native %d texel %d,%d: %s, decoder %s", format, native,
					static_cast<int>( points[i].z ), static_cast<int>( points[i].w ), Text( sampled[i] ), Text( expected ) ) );
			}
		}
	}
	idD3D12TextureCheck check( gpu.device, gpu, gpu.barriers, gpu.graveyard );
	const d3d12TextureCheckResult_t decodedCheck = check.Run( decoding );
	Check( decodedCheck.subresources == 2 && decodedCheck.mismatches == 0, "the decoded copies read back as the decoder wrote them" );
	const d3d12TextureCheckResult_t nativeCheck = check.Run( gpu.textures );
	Check( nativeCheck.subresources == 2 && nativeCheck.mismatches == 0, "the blocks read back as uploaded" );
	gpu.WaitIdle();
	decoding.DestroyAll();
	CheckNoLayerErrors( gpu );
}

void Test_WarpUnalignedBlocks() {
	if ( const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 ) ) {
		UnalignedBlocks( *gpu );
	}
}

void Test_GpuUnalignedBlocks() {
	if ( const std::unique_ptr<idTestGpu> gpu = CreateHardware( 16 << 20 ) ) {
		UnalignedBlocks( *gpu );
	}
}

// GL's face order and orientation: face f texel ( i, j ) is what the GL direction of its centre samples.
[[nodiscard]] idVec3 CubeDirection( int face, float s, float t ) {
	const float sc = s * 2.0f - 1.0f;
	const float tc = t * 2.0f - 1.0f;
	switch ( face ) {
		case 0:		return idVec3( 1.0f, -tc, -sc );
		case 1:		return idVec3( -1.0f, -tc, sc );
		case 2:		return idVec3( sc, 1.0f, tc );
		case 3:		return idVec3( sc, -1.0f, -tc );
		case 4:		return idVec3( sc, -tc, 1.0f );
		default:	return idVec3( -sc, -tc, -1.0f );
	}
}

void Test_WarpCubeFaces() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 );
	if ( !gpu ) {
		return;
	}
	idTestSampler sampler( *gpu );
	if ( !sampler.Ready() ) {
		return;
	}
	gpu->textures.CreateDefaults();
	const textureHandle_t handle = gpu->textures.Create( Opts( FMT_RGBA8, 2, 0, 2, TT_CUBIC ), textureRole_t::SAMPLED, POINT_CLAMP, "test cube" );
	if ( !Check( handle != textureHandle_t::NONE, "Create a cube" ) ) {
		return;
	}
	const d3d12Texture_t & cube = gpu->textures.Get( handle );
	Check( cube.surface.faces == 6 && cube.surface.width == 2 && cube.surface.height == 2 && cube.surface.levels == 2, "6 faces of 2 x 2, 2 levels" );
	for ( int face = 0; face < 6; face++ ) {
		byte texels[4][4];
		for ( int i = 0; i < 4; i++ ) {
			texels[i][0] = static_cast<byte>( 40 * face + 10 * i );
			texels[i][1] = static_cast<byte>( 255 - 30 * face );
			texels[i][2] = static_cast<byte>( 60 * i );
			texels[i][3] = 255;
		}
		gpu->textures.Upload( handle, 0, face, 2, 2, texels, 0 );
		const byte level1[4] = { static_cast<byte>( face ), 1, 2, 255 };
		gpu->textures.Upload( handle, 1, face, 1, 1, level1, 0 );
	}
	gpu->textures.MakeReadable( handle );
	const D3D12_SAMPLER_DESC point = SamplerOf( samplerFilter_t::NEAREST, samplerAddress_t::CLAMP );
	for ( int firstFace = 0; firstFace < 6; firstFace += 4 ) {
		std::vector<idVec4> points;
		std::vector<idVec4> expected;
		for ( int face = firstFace; face < Min( firstFace + 4, 6 ); face++ ) {
			for ( int i = 0; i < 4; i++ ) {
				const idVec3 direction = CubeDirection( face, ( ( i & 1 ) + 0.5f ) / 2.0f, ( ( i >> 1 ) + 0.5f ) / 2.0f );
				points.emplace_back( direction.x, direction.y, direction.z, 0.0f );
				expected.emplace_back( ( 40 * face + 10 * i ) / 255.0f, ( 255 - 30 * face ) / 255.0f, ( 60 * i ) / 255.0f, 1.0f );
			}
		}
		const std::vector<idVec4> sampled = sampler.Sample( gpu->textures.SampledView( handle, textureKind_t::TEX_CUBE ), textureKind_t::TEX_CUBE, point,
			points );
		for ( size_t i = 0; i < sampled.size(); i++ ) {
			Check( Near( sampled[i], expected[i], 0.5f / 255.0f ), va( "face %d texel %d: %s, expected %s", firstFace + static_cast<int>( i ) / 4,
				static_cast<int>( i ) % 4, Text( sampled[i] ), Text( expected[i] ) ) );
		}
	}
	const D3D12_SAMPLER_DESC mips = SamplerOf( samplerFilter_t::LINEAR_MIP_NEAREST, samplerAddress_t::CLAMP );
	const idVec4 level1[] = { idVec4( 0.0f, 0.0f, -1.0f, 1.0f ), idVec4( 1.0f, 0.0f, 0.0f, 1.0f ) };
	const std::vector<idVec4> sampledLevel = sampler.Sample( gpu->textures.SampledView( handle, textureKind_t::TEX_CUBE ), textureKind_t::TEX_CUBE, mips,
		level1 );
	Check( sampledLevel.size() == 2 && Near( sampledLevel[0], idVec4( 5 / 255.0f, 1 / 255.0f, 2 / 255.0f, 1.0f ), 0.5f / 255.0f )
		&& Near( sampledLevel[1], idVec4( 0.0f, 1 / 255.0f, 2 / 255.0f, 1.0f ), 0.5f / 255.0f ), "level 1 of -Z and +X" );

	// R11: the fallbacks, and a view of the other kind, read opaque black
	const idVec4 centre[] = { idVec4( 0.5f, 0.5f, 1.0f, 0.0f ) };
	const idVec4 black( 0.0f, 0.0f, 0.0f, 1.0f );
	const std::vector<idVec4> none2D = sampler.Sample( gpu->textures.SampledView( textureHandle_t::NONE, textureKind_t::TEX_2D ), textureKind_t::TEX_2D,
		point, centre );
	const std::vector<idVec4> cubeAs2D = sampler.Sample( gpu->textures.SampledView( handle, textureKind_t::TEX_2D ), textureKind_t::TEX_2D, point, centre );
	const std::vector<idVec4> noneCube = sampler.Sample( gpu->textures.SampledView( textureHandle_t::NONE, textureKind_t::TEX_CUBE ),
		textureKind_t::TEX_CUBE, point, centre );
	Check( none2D.size() == 1 && Near( none2D[0], black, 0.0f ), va( "NONE as 2D: %s", none2D.empty() ? "-" : Text( none2D[0] ) ) );
	Check( cubeAs2D.size() == 1 && Near( cubeAs2D[0], black, 0.0f ), "a cube bound as 2D samples the black 2D fallback" );
	Check( noneCube.size() == 1 && Near( noneCube[0], black, 0.0f ), "NONE as a cube" );
	CheckNoLayerErrors( *gpu );
}

// BC levels under 4 pixels come as whole blocks, a cube's with width 4; mips and the round trip of every subresource.
void Test_WarpTextureLevels() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 );
	if ( !gpu ) {
		return;
	}
	idTestSampler sampler( *gpu );
	if ( !sampler.Ready() ) {
		return;
	}
	gpu->textures.RecordUploads( true );
	const textureHandle_t bc = gpu->textures.Create( Opts( FMT_DXT1, 8, 8, 4 ), textureRole_t::SAMPLED, POINT_CLAMP, "test bc levels" );
	const textureHandle_t rgba = gpu->textures.Create( Opts( FMT_RGBA8, 8, 4, 4 ), textureRole_t::SAMPLED, POINT_CLAMP, "test rgba levels" );
	const textureHandle_t lum = gpu->textures.Create( Opts( FMT_LUM8, 5, 3, 1 ), textureRole_t::SAMPLED, POINT_CLAMP, "test pitch" );
	if ( !Check( bc != textureHandle_t::NONE && rgba != textureHandle_t::NONE && lum != textureHandle_t::NONE, "Create" ) ) {
		return;
	}
	for ( int level = 0; level < 4; level++ ) {
		const int size = Max( 4, 8 >> level );
		const std::vector<byte> blocks = Pattern( static_cast<size_t>( size / 4 ) * ( size / 4 ) * 8, 30 + level );
		gpu->textures.Upload( bc, level, 0, size, size, blocks.data(), 0 );
		const byte colour[4] = { static_cast<byte>( 50 * level ), 7, 9, 255 };
		std::vector<byte> rgbaLevel;
		for ( int i = 0; i < Max( 1, 8 >> level ) * Max( 1, 4 >> level ); i++ ) {
			rgbaLevel.insert( rgbaLevel.end(), colour, colour + 4 );
		}
		gpu->textures.Upload( rgba, level, 0, Max( 1, 8 >> level ), Max( 1, 4 >> level ), rgbaLevel.data(), 0 );
	}
	// a cinematic plane: 5 x 3 of a buffer 8 pixels wide
	const std::vector<byte> plane = Pattern( 8 * 3, 40 );
	gpu->textures.Upload( lum, 0, 0, 5, 3, plane.data(), 8 );
	gpu->textures.MakeReadable( rgba );
	gpu->textures.MakeReadable( lum );

	const D3D12_SAMPLER_DESC mips = SamplerOf( samplerFilter_t::LINEAR_MIP_NEAREST, samplerAddress_t::CLAMP );
	std::vector<idVec4> levelPoints;
	for ( int level = 0; level < 4; level++ ) {
		levelPoints.emplace_back( 0.5f, 0.5f, 0.0f, static_cast<float>( level ) );
	}
	const std::vector<idVec4> levels = sampler.Sample( gpu->textures.SampledView( rgba, textureKind_t::TEX_2D ), textureKind_t::TEX_2D, mips, levelPoints );
	for ( size_t level = 0; level < levels.size(); level++ ) {
		Check( Near( levels[level], idVec4( 50 * level / 255.0f, 7 / 255.0f, 9 / 255.0f, 1.0f ), 0.5f / 255.0f ), va( "RGBA8 level %d: %s",
			static_cast<int>( level ), Text( levels[level] ) ) );
	}
	std::vector<idVec4> pitchPoints;
	for ( int i = 0; i < 15; i++ ) {
		pitchPoints.emplace_back( ( i % 5 + 0.5f ) / 5.0f, ( i / 5 + 0.5f ) / 3.0f, 0.0f, 0.0f );
	}
	const std::vector<idVec4> pitched = sampler.Sample( gpu->textures.SampledView( lum, textureKind_t::TEX_2D ), textureKind_t::TEX_2D,
		SamplerOf( samplerFilter_t::NEAREST, samplerAddress_t::CLAMP ), pitchPoints );
	for ( size_t i = 0; i < pitched.size(); i++ ) {
		const float l = Unorm8( plane[( i / 5 ) * 8 + i % 5] );
		Check( Near( pitched[i], idVec4( l, l, l, 1.0f ), 0.5f / 255.0f ), va( "LUM8 with a pitch, texel %d: %s", static_cast<int>( i ), Text( pitched[i] ) ) );
	}

	idD3D12TextureCheck check( gpu->device, *gpu, gpu->barriers, gpu->graveyard );
	const d3d12TextureCheckResult_t result = check.Run( gpu->textures );
	Check( result.textures == 3 && result.subresources == 9 && result.mismatches == 0 && result.unrecorded == 0,
		va( "the round trip: %d textures, %d subresources, %d mismatches", result.textures, result.subresources, result.mismatches ) );
	gpu->textures.Get( rgba ).uploads[2].hash ^= 1;
	const d3d12TextureCheckResult_t broken = check.Run( gpu->textures );
	Check( broken.mismatches == 1, "a wrong hash is one mismatch" );
	CheckNoLayerErrors( *gpu );
}

// R3: the resource and its view slot die after an explicit wait for the open list; the slot is reused.
void Test_WarpTextureGraves() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 );
	if ( !gpu ) {
		return;
	}
	const textureHandle_t first = gpu->textures.Create( Opts( FMT_RGBA8, 64, 64, 7 ), textureRole_t::SAMPLED, POINT_CLAMP, "test grave" );
	const textureHandle_t depth = gpu->textures.Create( Opts( FMT_RGBA8, 32, 32, 1 ), textureRole_t::DEPTH_COPY, POINT_CLAMP, "test depth copy" );
	if ( !Check( first != textureHandle_t::NONE && depth != textureHandle_t::NONE, "Create" ) ) {
		return;
	}
	Check( gpu->textures.Get( depth ).surface.rtv.IsValid() && !gpu->textures.Get( first ).surface.rtv.IsValid(), "only DEPTH_COPY has an RTV" );
	const uint64 bytes = gpu->textures.Bytes();
	Check( gpu->textures.Num() == 2 && bytes >= 64 * 64 * 4 + 32 * 32 * 4, va( "%llu bytes", static_cast<unsigned long long>( bytes ) ) );
	const d3d12Descriptor_t slot = gpu->textures.Get( first ).surface.srv;
	const std::vector<byte> data = Pattern( 64 * 64 * 4, 50 );
	gpu->textures.Upload( first, 0, 0, 64, 64, data.data(), 0 );
	gpu->textures.Destroy( first );
	gpu->textures.Destroy( depth );
	Check( gpu->textures.Find( first ) == nullptr && gpu->textures.Num() == 0 && gpu->textures.Bytes() == 0, "the handles die at once" );
	Check( gpu->graveyard.Pending() == 6, va( "%d graves: two resources, two views, an RTV and the sampler they held", gpu->graveyard.Pending() ) );
	const textureHandle_t early = gpu->textures.Create( Opts( FMT_RGBA8, 4, 4, 1 ), textureRole_t::SAMPLED, POINT_CLAMP, "test early" );
	Check( gpu->textures.Get( early ).surface.srv != slot, "the slot is not reused before the wait" );
	gpu->textures.Destroy( early );
	gpu->WaitIdle();
	Check( gpu->graveyard.Pending() == 0, "collected after the wait" );
	const textureHandle_t again = gpu->textures.Create( Opts( FMT_RGBA8, 4, 4, 1 ), textureRole_t::SAMPLED, POINT_CLAMP, "test again" );
	Check( gpu->textures.Get( again ).surface.srv == slot, "the lowest slot again" );
	Check( gpu->textures.Create( Opts( FMT_NONE, 4, 4, 1 ), textureRole_t::SAMPLED, POINT_CLAMP, "test none" ) == textureHandle_t::NONE, "FMT_NONE" );
	CheckNoLayerErrors( *gpu );
}

// ---- base/shaders through DXC -------------------------------------------------------------------------------

[[nodiscard]] std::optional<idStr> ReadDiskText( const idStr & path ) {
	FILE * file = nullptr;
	if ( fopen_s( &file, path.c_str(), "rb" ) != 0 || file == nullptr ) {
		return std::nullopt;
	}
	idStr text;
	char buffer[4096];
	for ( size_t read; ( read = fread( buffer, 1, sizeof( buffer ), file ) ) > 0; ) {
		text.Append( buffer, static_cast<int>( read ) );
	}
	fclose( file );
	return text;
}

[[nodiscard]] idStr WithoutCarriageReturns( const idStr & text ) {
	idStr out = text;
	out.Replace( "\r", "" );
	return out;
}

[[nodiscard]] idStr RepoPath( const char * relative ) {
	return idStr( D3D12_TESTS_SOURCE_DIR ) + "/" + relative;
}

[[nodiscard]] std::unique_ptr<idD3D12Library> LoadLibraries() {
	auto library = idD3D12Library::Load();
	if ( !library ) {
		Check( false, va( "idD3D12Library::Load: %s", library.error().c_str() ) );
		return nullptr;
	}
	return std::make_unique<idD3D12Library>( std::move( *library ) );
}

[[nodiscard]] idStr ParmList( const shaderStageInfo_t & info ) {
	if ( info.numParms == 0 ) {
		return "-";
	}
	idStr list;
	for ( int i = 0; i < info.numParms; i++ ) {
		list += va( ( i > 0 ) ? ",%d" : "%d", info.parms[i] );
	}
	return list;
}

// The value of key=value in a renderProgInfo line.
[[nodiscard]] idStr InfoField( const idStr & line, const char * key ) {
	const idStr prefix = idStr( " " ) + key + "=";
	const int start = line.Find( prefix );
	if ( start < 0 ) {
		return idStr();
	}
	idStr value = line.c_str() + start + prefix.Length();
	const int end = value.Find( ' ' );
	if ( end >= 0 ) {
		value.CapLength( end );
	}
	return value;
}

// A path under base/ from the repo, as fileSystem gives it to the engine.
[[nodiscard]] d3d12ShaderReader_t RepoReader() {
	return []( const char * path ) { return ReadDiskText( RepoPath( idStr( "base/" ) + path ) ); };
}

// A stage by the name shared code gives it ("gui.vfp", "depth_skinned"), from base/shaders.
[[nodiscard]] std::expected<d3d12ShaderStage_t, idStr> RepoStage( idD3D12HlslCompiler & compiler, const char * name, shaderStageType_t type ) {
	const std::optional<d3d12HlslSource_t> source = D3D12_FindHlslSource( name, compiler.Reader() );
	if ( !source ) {
		return std::unexpected( idStr( va( "%s: no HLSL", name ) ) );
	}
	return D3D12_BuildHlslStage( compiler, type, name, *source );
}

// Every stage of a shaders/*.hlsl, the SKINNED variant included.
[[nodiscard]] std::vector<d3d12HlslEntry_t> HlslStages( const idD3D12Library & library, const char * file ) {
	const std::optional<d3d12HlslSource_t> plain = D3D12_FindHlslSource( file, RepoReader() );
	if ( !Check( plain.has_value(), va( "%s: not found", file ) ) ) {
		return {};
	}
	auto entries = D3D12_HlslEntries( library, *plain, RepoReader() );
	if ( !Check( entries.has_value(), va( "%s: %s", file, entries ? "" : entries.error().c_str() ) ) ) {
		return {};
	}
	return std::move( *entries );
}

// The masks are the b3 members a stage reads, as GL's are its active sampler uniforms: a texture declared
// but not sampled counts for nothing. A register binding of a texture is refused.
void Test_HlslBindless() {
	const std::unique_ptr<idD3D12Library> library = LoadLibraries();
	if ( !library ) {
		return;
	}
	idD3D12HlslCompiler compiler( *library, RepoReader(), false );
	const auto source = []( const char * text ) {
		d3d12HlslSource_t made;
		made.name = "test_bindless";
		made.path = "shaders/test_bindless.hlsl";
		made.text = text;
		return made;
	};
	const auto built = compiler.Build( source(
		"#include \"include/bindless.hlsli\"\n"
		"TEXTURE2D( Texture2D, bumpMap, 0 );\n"
		"TEXTURECUBE( TextureCube, environmentMap, 2 );\n"
		"TEXTURE2D( Texture2D, unusedMap, 3 );\n"
		"SAMPLER( bumpSampler, 0 );\n"
		"SAMPLER( environmentSampler, 2 );\n"
		"float4 PSMain( float4 position : SV_Position, float3 direction : TEXCOORD0 ) : SV_Target {\n"
		"	return bumpMap.Sample( bumpSampler, direction.xy ) + environmentMap.Sample( environmentSampler, direction );\n"
		"}\n" ), shaderStageType_t::FRAGMENT, hlslParms_t::ANY );
	if ( Check( built.has_value(), built ? "" : built.error().c_str() ) ) {
		Check( built->reflection.samplerMask == 0x5 && built->reflection.cubeMask == 0x4, va( "samplers 0x%x, cubes 0x%x: not 0x5 and 0x4",
			built->reflection.samplerMask, built->reflection.cubeMask ) );
	}
	const auto bound = compiler.Build( source(
		"Texture2D map : register( t0 );\n"
		"SamplerState mapSampler : register( s0 );\n"
		"float4 PSMain( float4 position : SV_Position ) : SV_Target { return map.Sample( mapSampler, position.xy ); }\n" ),
		shaderStageType_t::FRAGMENT, hlslParms_t::ANY );
	Check( !bound && bound.error().Find( "bindless.hlsli" ) >= 0, bound ? "a texture at t0 compiled" : bound.error().c_str() );
	// a draw fills sampler<n> from the texture at n: a program reading another slot's sampler is refused
	const auto lone = compiler.Build( source(
		"#include \"include/bindless.hlsli\"\n"
		"TEXTURE2D( Texture2D, map, 1 );\n"
		"SAMPLER( mapSampler, 0 );\n"
		"float4 PSMain( float4 position : SV_Position ) : SV_Target { return map.Sample( mapSampler, position.xy ); }\n" ),
		shaderStageType_t::FRAGMENT );
	Check( !lone && lone.error().Find( "SAMPLER( ..., 0 )" ) >= 0, lone ? "sampler 0 with texture 1 compiled" : lone.error().c_str() );
}

// The preprocess sees the stage's profile: a file that parts its stages by __SHADER_TARGET_STAGE builds both.
void Test_HlslTargetMacros() {
	const std::unique_ptr<idD3D12Library> library = LoadLibraries();
	if ( !library ) {
		return;
	}
	idD3D12HlslCompiler compiler( *library, RepoReader(), false );
	d3d12HlslSource_t source;
	source.name = "test_target";
	source.path = "shaders/test_target.hlsl";
	source.text =
		"#if __SHADER_TARGET_STAGE == __SHADER_STAGE_VERTEX\n"
		"float4 VSMain() : SV_Position { return 0; }\n"
		"#elif __SHADER_TARGET_STAGE == __SHADER_STAGE_PIXEL\n"
		"float4 PSMain() : SV_Target { return 1; }\n"
		"#else\n"
		"#error no stage\n"
		"#endif\n";
	for ( const shaderStageType_t stage : { shaderStageType_t::VERTEX, shaderStageType_t::FRAGMENT } ) {
		const auto built = compiler.Build( source, stage, hlslParms_t::ANY );
		Check( built.has_value(), va( "%s: %s", D3D12_StageEntry( stage ), built ? "" : built.error().c_str() ) );
	}
	Check( compiler.Compiled() == 2, va( "two texts, two DXC runs: %d", compiler.Compiled() ) );
}

// Every stage of the backend's own passes builds from base/shaders/internal.
void Test_HlslInternalStages() {
	const std::unique_ptr<idD3D12Library> library = LoadLibraries();
	if ( !library ) {
		return;
	}
	idD3D12HlslCompiler compiler( *library, RepoReader(), false );
	std::vector<d3d12HlslSource_t> sources;
	for ( const d3d12InternalStage_t & stage : D3D12_InternalStages() ) {
		std::optional<d3d12HlslSource_t> source = D3D12_InternalSource( stage, RepoReader() );
		if ( Check( source.has_value(), va( "shaders/internal/%s.hlsl", stage.file ) ) ) {
			sources.push_back( std::move( *source ) );
		}
	}
	if ( sources.size() != D3D12_InternalStages().size() ) {
		return;
	}
	std::vector<d3d12HlslRequest_t> requests;
	for ( size_t i = 0; i < sources.size(); i++ ) {
		requests.push_back( { &sources[i], D3D12_InternalStages()[i].type, hlslParms_t::ANY } );
	}
	const std::vector<d3d12HlslResult_t> built = compiler.BuildAll( requests );
	for ( size_t i = 0; i < built.size(); i++ ) {
		Check( built[i].has_value(), va( "%s: %s", sources[i].name.c_str(), built[i] ? "" : built[i].error().c_str() ) );
	}
}

// A compute stage binds b0 and b1 (at most the root constants) and reads its resources through ResourceDescriptorHeap.
void Test_HlslCompute() {
	const std::unique_ptr<idD3D12Library> library = LoadLibraries();
	if ( !library ) {
		return;
	}
	idD3D12HlslCompiler compiler( *library, RepoReader(), false );
	const auto source = []( const char * text ) {
		d3d12HlslSource_t made;
		made.name = "test_compute";
		made.path = "shaders/test_compute.hlsl";
		made.text = text;
		return made;
	};
	const auto built = compiler.Build( source(
		"cbuffer Parms : register( b0 ) { uint target; };\n"
		"cbuffer Level : register( b1 ) { uint level; };\n"
		"[numthreads( 8, 4, 1 )]\n"
		"void CSMain( uint3 id : SV_DispatchThreadID ) {\n"
		"	RWTexture2D<float4> output = ResourceDescriptorHeap[target];\n"
		"	output[id.xy] = float4( level, 0.0, 0.0, 1.0 );\n"
		"}\n" ), shaderStageType_t::COMPUTE, hlslParms_t::ANY );
	if ( Check( built.has_value(), built ? "" : built.error().c_str() ) ) {
		const std::array<uint32, 3> & group = built->reflection.threadGroup;
		Check( group[0] == 8 && group[1] == 4 && group[2] == 1, va( "numthreads %u %u %u, not 8 4 1", group[0], group[1], group[2] ) );
	}
	const auto bound = compiler.Build( source(
		"RWTexture2D<float4> output : register( u0 );\n"
		"[numthreads( 8, 8, 1 )]\n"
		"void CSMain( uint3 id : SV_DispatchThreadID ) { output[id.xy] = 1.0; }\n" ), shaderStageType_t::COMPUTE, hlslParms_t::ANY );
	Check( !bound && bound.error().Find( "ResourceDescriptorHeap" ) >= 0, bound ? "a UAV at u0 compiled" : bound.error().c_str() );
	const auto large = compiler.Build( source(
		"cbuffer Parms : register( b0 ) { uint target; };\n"
		"cbuffer Level : register( b1 ) { uint4 a; uint4 b; uint c; };\n"
		"[numthreads( 8, 8, 1 )]\n"
		"void CSMain( uint3 id : SV_DispatchThreadID ) {\n"
		"	RWTexture2D<float4> output = ResourceDescriptorHeap[target];\n"
		"	output[id.xy] = float4( a.x, b.x, c, 1.0 );\n"
		"}\n" ), shaderStageType_t::COMPUTE, hlslParms_t::ANY );
	Check( !large && large.error().Find( "root constants" ) >= 0, large ? "36 bytes at b1 compiled" : large.error().c_str() );
}

// Stages from the preprocessed text: names in comments, and a SKINNED that changes nothing, add none.
void Test_HlslEntries() {
	const std::unique_ptr<idD3D12Library> library = LoadLibraries();
	if ( !library ) {
		return;
	}
	const d3d12ShaderReader_t reader = []( const char * path ) -> std::optional<idStr> {
		if ( idStr::Icmp( path, "shaders/include/a.hlsli" ) == 0 ) {
			return idStr( "#if SKINNED\nfloat4 Skin( float4 p ) { return p * 2.0; }\n#endif\n" );
		}
		return std::nullopt;
	};
	const auto Entries = [&]( const char * text ) {
		d3d12HlslSource_t source;
		source.name = "test";
		source.path = "shaders/test.hlsl";
		source.text = text;
		idStr list;
		if ( const auto entries = D3D12_HlslEntries( *library, source, reader ) ) {
			for ( const auto & [variant, stage] : *entries ) {
				list += va( "%s%s:%s", list.IsEmpty() ? "" : " ", variant.name.c_str(), ( stage == shaderStageType_t::VERTEX ) ? "VS" : "PS" );
			}
		} else {
			list = entries.error();
		}
		return list;
	};
	const idStr vsOnly = Entries( "// no PSMain here, and SKINNED is a word\nfloat4 VSMain() : SV_Position { return 0; }\n" );
	Check( vsOnly == "test:VS", va( "a comment adds nothing: %s", vsOnly.c_str() ) );
	const idStr both = Entries( "float4 VSMain () : SV_Position { return 0; }\nfloat4 PSMainColor() { return 1; }\nfloat4 PSMain() : SV_Target { return 1; }\n" );
	Check( both == "test:VS test:PS", va( "whole names before a '(': %s", both.c_str() ) );
	const idStr skinned = Entries( "#include \"include/a.hlsli\"\nfloat4 VSMain( float4 p : POSITION ) : SV_Position { return p; }\n" );
	Check( skinned == "test:VS test_skinned:VS", va( "SKINNED in an include makes a variant: %s", skinned.c_str() ) );
	const idStr unchanged = Entries( "#if SKINNED\n#endif\nfloat4 VSMain() : SV_Position { return 0; }\n" );
	Check( unchanged == "test:VS", va( "an empty SKINNED block makes none: %s", unchanged.c_str() ) );
}

// DXC compiles every stage of base/shaders without a warning; the backend can fill every binding it has.
void Test_HlslSourcesCompile() {
	const std::unique_ptr<idD3D12Library> library = LoadLibraries();
	if ( !library ) {
		return;
	}
	idD3D12HlslCompiler compiler( *library, RepoReader(), false );
	int stages = 0;
	for ( const std::filesystem::directory_entry & entry : std::filesystem::directory_iterator( RepoPath( "base/shaders" ).c_str() ) ) {
		if ( !entry.is_regular_file() || entry.path().extension() != ".hlsl" ) {
			continue;
		}
		for ( const auto & [source, type] : HlslStages( *library, entry.path().filename().string().c_str() ) ) {
			stages++;
			const char * entryPoint = D3D12_StageEntry( type );
			std::vector<const char *> defines;
			for ( int i = 0; i < source.defines.Num(); i++ ) {
				defines.push_back( source.defines[i].c_str() );
			}
			const d3d12ShaderReader_t reader = RepoReader();
			const auto preprocessed = D3D12_DxcPreprocess( *library, std::string_view( source.text.c_str(), source.text.Length() ), source.path,
				defines, reader );
			if ( !Check( preprocessed.has_value(), va( "%s %s: %s", source.name.c_str(), entryPoint, preprocessed ? "" : preprocessed.error().c_str() ) ) ) {
				continue;
			}
			const auto output = D3D12_DxcCompile( *library, std::string_view( preprocessed->c_str(), preprocessed->Length() ), source.path, entryPoint,
				( type == shaderStageType_t::VERTEX ) ? "vs_6_6" : "ps_6_6" );
			if ( Check( output.has_value(), va( "%s %s: %s", source.name.c_str(), entryPoint, output ? "" : output.error().c_str() ) ) ) {
				Check( output->warnings.IsEmpty(), va( "%s %s: %s", source.name.c_str(), entryPoint, output->warnings.c_str() ) );
				Check( D3D12_DxilSigned( output->code ), va( "%s %s: signed", source.name.c_str(), entryPoint ) );
			}
			const auto built = compiler.Build( source, type );
			Check( built.has_value(), va( "%s %s: %s", source.name.c_str(), entryPoint, built ? "" : built.error().c_str() ) );
		}
	}
	printf( "  %d stages\n", stages );
	Check( stages > 0, "base/shaders has stages" );
}

// GL's renderProgInfo of mars_city1 against base/shaders: every program has its stages there, with the same parms
// in the same order, masks and joints, attribMask a superset, and the PS reads what the VS writes.
void Test_HlslProgramsMatchGl() {
	const std::unique_ptr<idD3D12Library> library = LoadLibraries();
	const std::optional<idStr> reference = ReadDiskText( RepoPath( "neo/tests/d3d12_golden/renderProgInfo-gl.txt" ) );
	if ( !library || !Check( reference.has_value(), "no neo/tests/d3d12_golden/renderProgInfo-gl.txt" ) ) {
		return;
	}
	idD3D12HlslCompiler compiler( *library, RepoReader(), false );
	const idStr text = WithoutCarriageReturns( *reference );
	int programs = 0;
	for ( int start = 0, newline; ( newline = text.Find( '\n', start ) ) >= 0; start = newline + 1 ) {
		const idStr line = text.Mid( start, newline - start );
		if ( line.IsEmpty() || line[0] == '#' ) {
			continue;
		}
		const idStr vertexName = InfoField( line, "vs" );
		const idStr fragmentName = InfoField( line, "fs" );
		programs++;
		const auto vertex = RepoStage( compiler, vertexName, shaderStageType_t::VERTEX );
		const auto fragment = ( fragmentName != "-" ) ? RepoStage( compiler, fragmentName, shaderStageType_t::FRAGMENT )
			: std::expected<d3d12ShaderStage_t, idStr>( std::unexpected( idStr( "-" ) ) );
		if ( !vertex || ( !fragment && fragmentName != "-" ) ) {
			Check( false, va( "%s|%s: %s", vertexName.c_str(), fragmentName.c_str(), !vertex ? vertex.error().c_str() : fragment.error().c_str() ) );
			continue;
		}
		auto program = D3D12_LinkProgram( *vertex, fragment ? &*fragment : nullptr );
		if ( !Check( program.has_value(), va( "%s|%s: link: %s", vertexName.c_str(), fragmentName.c_str(), program ? "" : program.error().c_str() ) ) ) {
			continue;
		}
		const programInfo_t & info = program->info;
		const uint16 glAttribs = static_cast<uint16>( strtoul( InfoField( line, "attribMask" ), nullptr, 16 ) );
		const idStr d3d12 = va( "samplerMask=0x%04x cubeMask=0x%04x usesJoints=%d vsParms=%s fsParms=%s", info.samplerMask, info.cubeMask,
			info.usesJoints ? 1 : 0, ParmList( vertex->info ).c_str(), fragment ? ParmList( fragment->info ).c_str() : "-" );
		const idStr gl = va( "samplerMask=%s cubeMask=%s usesJoints=%s vsParms=%s fsParms=%s", InfoField( line, "samplerMask" ).c_str(),
			InfoField( line, "cubeMask" ).c_str(), InfoField( line, "usesJoints" ).c_str(), InfoField( line, "vsParms" ).c_str(),
			InfoField( line, "fsParms" ).c_str() );
		Check( d3d12 == gl, va( "%s|%s: HLSL %s, GL %s", vertexName.c_str(), fragmentName.c_str(), d3d12.c_str(), gl.c_str() ) );
		Check( ( info.attribMask & glAttribs ) == glAttribs, va( "%s|%s: attribMask 0x%04x lacks some of GL's 0x%04x", vertexName.c_str(),
			fragmentName.c_str(), info.attribMask, glAttribs ) );
	}
	printf( "  %d programs\n", programs );
	Check( programs == 43, va( "renderProgInfo-gl.txt has %d programs, 43 expected", programs ) );
}

// The program table against base/shaders and GL: each program has its file with exactly the stages and the
// skinned variant the table lists, each file is a program, each builtin is made once, and every stage GL
// loaded on mars_city1 is in the table.
void Test_HlslProgramTable() {
	const std::optional<renderProgStage_t> skinned = R_FindRenderProg( "INTERACTION_skinned.vfp" );
	Check( skinned && skinned->skinned && idStr::Cmp( skinned->program->name, "interaction" ) == 0, "a skinned name, case ignored" );
	Check( !R_FindRenderProg( "heatHaze_skinned.vfp" ) && !R_FindRenderProg( "ambientLight.vfp" ), "names the table lacks" );
	Check( R_RenderProgHasStage( "shadow.vp", shaderStageType_t::VERTEX ) && !R_RenderProgHasStage( "shadow.vp", shaderStageType_t::FRAGMENT ),
		"shadow is vertex only" );

	std::array<int, static_cast<size_t>( renderProgBuiltin_t::COUNT )> builtins = {};
	for ( const renderProgDef_t & program : R_RenderProgs() ) {
		for ( const renderProgBuiltin_t builtin : { program.builtin, program.skinned } ) {
			if ( builtin != renderProgBuiltin_t::NONE ) {
				builtins[static_cast<size_t>( builtin )]++;
			}
		}
		const idStr stage = R_RenderProgStageName( program, false );
		const std::optional<renderProgStage_t> found = R_FindRenderProg( stage.c_str() );
		Check( found && found->program == &program && !found->skinned, va( "%s finds its row", stage.c_str() ) );
	}
	for ( size_t i = 0; i < builtins.size(); i++ ) {
		Check( builtins[i] == 1, va( "builtin %zu is made %d times", i, builtins[i] ) );
	}

	const std::unique_ptr<idD3D12Library> library = LoadLibraries();
	if ( library ) {
		for ( const renderProgDef_t & program : R_RenderProgs() ) {
			idStr have;
			for ( const auto & [source, type] : HlslStages( *library, va( "%s.hlsl", program.name ) ) ) {
				have += va( "%s%s:%s", have.IsEmpty() ? "" : " ", source.name.c_str(), ( type == shaderStageType_t::VERTEX ) ? "VS" : "PS" );
			}
			idStr want = va( "%s:VS", program.name );
			if ( program.fragment ) {
				want += va( " %s:PS", program.name );
			}
			if ( program.skinned != renderProgBuiltin_t::NONE ) {
				want += va( " %s_skinned:VS", program.name );
			}
			// a fragment stage SKINNED does not change is the plain one
			const idStr skinnedFragment = va( " %s_skinned:PS", program.name );
			if ( program.skinned != renderProgBuiltin_t::NONE && program.fragment && have.Find( skinnedFragment ) >= 0 ) {
				want += skinnedFragment;
			}
			Check( have == want, va( "shaders/%s.hlsl has %s, the table %s", program.name, have.c_str(), want.c_str() ) );
		}
	}
	for ( const std::filesystem::directory_entry & entry : std::filesystem::directory_iterator( RepoPath( "base/shaders" ).c_str() ) ) {
		if ( entry.is_regular_file() && entry.path().extension() == ".hlsl" ) {
			const std::string stem = entry.path().stem().string();
			const std::optional<renderProgStage_t> found = R_FindRenderProg( stem );
			Check( found && !found->skinned, va( "shaders/%s.hlsl is no program of the table", stem.c_str() ) );
		}
	}

	const std::optional<idStr> reference = ReadDiskText( RepoPath( "neo/tests/d3d12_golden/renderProgInfo-gl.txt" ) );
	if ( !Check( reference.has_value(), "no neo/tests/d3d12_golden/renderProgInfo-gl.txt" ) ) {
		return;
	}
	const idStr text = WithoutCarriageReturns( *reference );
	int glPrograms = 0;
	for ( int start = 0, newline; ( newline = text.Find( '\n', start ) ) >= 0; start = newline + 1 ) {
		const idStr line = text.Mid( start, newline - start );
		if ( line.IsEmpty() || line[0] == '#' ) {
			continue;
		}
		glPrograms++;
		const idStr vertex = InfoField( line, "vs" );
		const idStr fragment = InfoField( line, "fs" );
		Check( R_RenderProgHasStage( vertex.c_str(), shaderStageType_t::VERTEX ), va( "GL's %s is no vertex stage of the table", vertex.c_str() ) );
		Check( fragment == "-" || R_RenderProgHasStage( fragment.c_str(), shaderStageType_t::FRAGMENT ),
			va( "GL's %s is no fragment stage of the table", fragment.c_str() ) );
	}
	printf( "  %d programs in the table, the stages of GL's %d in it\n", static_cast<int>( R_RenderProgs().size() ), glPrograms );
}

[[nodiscard]] bool SameStageInfo( const shaderStageInfo_t & a, const shaderStageInfo_t & b ) {
	return a.numParms == b.numParms && memcmp( a.parms, b.parms, static_cast<size_t>( a.numParms ) ) == 0;
}

// BuildAll is Build one stage after another: every stage of base/shaders gives the same DXIL, parms and
// masks on all threads as on one; a text asked twice is one DXC run, and a broken stage fails alone.
void Test_HlslBuildAll() {
	const std::unique_ptr<idD3D12Library> library = LoadLibraries();
	if ( !library ) {
		return;
	}
	std::vector<d3d12HlslEntry_t> entries;
	for ( const std::filesystem::directory_entry & entry : std::filesystem::directory_iterator( RepoPath( "base/shaders" ).c_str() ) ) {
		if ( entry.is_regular_file() && entry.path().extension() == ".hlsl" ) {
			std::ranges::move( HlslStages( *library, entry.path().filename().string().c_str() ), std::back_inserter( entries ) );
		}
	}
	if ( !Check( entries.size() > 2, "the stages of base/shaders" ) ) {
		return;
	}
	d3d12HlslSource_t broken;
	broken.name = "test_broken";
	broken.path = "shaders/test_broken.hlsl";
	broken.text = "float4 PSMain() : SV_Target { return undefinedName; }\n";
	std::vector<d3d12HlslRequest_t> requests;
	for ( const d3d12HlslEntry_t & entry : entries ) {
		requests.push_back( { &entry.source, entry.stage } );
	}
	const size_t brokenIndex = requests.size() / 2;
	requests.insert( requests.begin() + static_cast<std::ptrdiff_t>( brokenIndex ), { &broken, shaderStageType_t::FRAGMENT } );
	requests.push_back( requests.front() );

	idD3D12HlslCompiler parallel( *library, RepoReader(), false );
	const std::vector<d3d12HlslResult_t> all = parallel.BuildAll( requests );
	idD3D12HlslCompiler serial( *library, RepoReader(), false, 1 );
	Check( all.size() == requests.size(), "a result per request" );
	for ( size_t i = 0; i < requests.size() && i < all.size(); i++ ) {
		const char * name = requests[i].source->name.c_str();
		if ( i == brokenIndex ) {
			Check( !all[i] && all[i].error().Find( "undefinedName" ) >= 0, "the broken stage fails with DXC's error" );
			continue;
		}
		const d3d12HlslResult_t one = serial.Build( *requests[i].source, requests[i].stage );
		if ( !Check( all[i].has_value() && one.has_value(), va( "%s: %s", name, !all[i] ? all[i].error().c_str() : !one ? one.error().c_str() : "" ) ) ) {
			continue;
		}
		Check( all[i]->code == one->code, va( "%s: the same DXIL", name ) );
		Check( SameStageInfo( all[i]->info, one->info ), va( "%s: the same parms", name ) );
		Check( all[i]->reflection.samplerMask == one->reflection.samplerMask && all[i]->reflection.attribMask == one->reflection.attribMask
			&& all[i]->reflection.usesJoints == one->reflection.usesJoints && all[i]->reflection.outputs.size() == one->reflection.outputs.size(),
			va( "%s: the same reflection", name ) );
	}
	Check( parallel.Compiled() < serial.Compiled(), va( "one DXC run per distinct text: %d runs for %d stages", parallel.Compiled(), serial.Compiled() ) );
	printf( "  %zu stages: %d DXC runs in %.0f ms, one after another %d in %.0f ms\n", requests.size(), parallel.Compiled(), parallel.Milliseconds(),
		serial.Compiled(), serial.Milliseconds() );
}

// Prebuild compiles every stage of the program table; CreateStage then takes it without DXC, with the parms
// a library without Prebuild gets, and compiles a stage asked for again.
void PrebuiltStages( idTestGpu & gpu ) {
	idD3D12HlslCompiler prebuiltCompiler( gpu.library, RepoReader(), false );
	idD3D12HlslCompiler plainCompiler( gpu.library, RepoReader(), false );
	idD3D12ShaderLibrary prebuilt( prebuiltCompiler, gpu.graveyard );
	idD3D12ShaderLibrary plain( plainCompiler, gpu.graveyard );
	prebuilt.Prebuild();
	const int runs = prebuiltCompiler.Compiled();
	int numStages = 0;
	for ( const renderProgDef_t & program : R_RenderProgs() ) {
		for ( const bool skinned : { false, true } ) {
			if ( skinned && program.skinned == renderProgBuiltin_t::NONE ) {
				continue;
			}
			const idStr name = R_RenderProgStageName( program, skinned );
			for ( const shaderStageType_t type : { shaderStageType_t::VERTEX, shaderStageType_t::FRAGMENT } ) {
				if ( type == shaderStageType_t::FRAGMENT && !program.fragment ) {
					continue;
				}
				numStages++;
				shaderStageInfo_t a = {}, b = {};
				const shaderHandle_t ha = prebuilt.CreateStage( type, name.c_str(), a );
				const shaderHandle_t hb = plain.CreateStage( type, name.c_str(), b );
				Check( ha != shaderHandle_t::NONE && hb != shaderHandle_t::NONE && SameStageInfo( a, b ), va( "%s %d: the parms", name.c_str(),
					static_cast<int>( type ) ) );
			}
		}
	}
	Check( prebuiltCompiler.Compiled() == runs, va( "%d stages without DXC after Prebuild: %d runs more", numStages, prebuiltCompiler.Compiled() - runs ) );
	Check( runs <= plainCompiler.Compiled(), va( "Prebuild: %d DXC runs, one stage at a time %d", runs, plainCompiler.Compiled() ) );
	shaderStageInfo_t again = {};
	Check( prebuilt.CreateStage( shaderStageType_t::VERTEX, "interaction.vfp", again ) != shaderHandle_t::NONE && again.numParms > 0
		&& prebuiltCompiler.Compiled() == runs + 1, "a stage asked for again is compiled" );
	printf( "  %d stages, %d DXC runs in %.0f ms; one at a time %d in %.0f ms\n", numStages, runs, prebuiltCompiler.Milliseconds(),
		plainCompiler.Compiled(), plainCompiler.Milliseconds() );
	prebuilt.Prebuild();
	prebuilt.DropPrebuilt();
	const int dropped = prebuiltCompiler.Compiled();
	shaderStageInfo_t late = {};
	Check( prebuilt.CreateStage( shaderStageType_t::VERTEX, "heatHaze.vfp", late ) != shaderHandle_t::NONE && prebuiltCompiler.Compiled() == dropped + 1,
		"after DropPrebuilt a stage is built again" );
	prebuilt.DestroyAll();
	plain.DestroyAll();
}

// A sampler slot lives while something holds it; the last Release buries it, and only the GPU passing the open
// list's fence lets another sampler have it. Many more samplers than slots, each held a while, fit.
void Test_WarpSamplerSlots() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 );
	if ( !gpu ) {
		return;
	}
	{
		idD3D12SamplerHeap heap( gpu->device, gpu->graveyard );
		const auto Packed = []( int i ) {
			return D3D12_PackSampler( { samplerFilter_t::LINEAR, samplerAddress_t::REPEAT, 1.0f, static_cast<float>( i ) / 256.0f } );
		};
		const uint32 slot = heap.Acquire( Packed( 1 ) );
		Check( heap.Acquire( Packed( 1 ) ) == slot && heap.Used() == 2, "the same sampler, the same slot" );
		heap.Release( slot );
		Check( heap.Used() == 2 && heap.Acquire( Packed( 1 ) ) == slot, "held once more, the slot stays" );
		heap.Release( slot );
		heap.Release( slot );
		Check( heap.Used() == 1 && heap.Acquire( Packed( 2 ) ) != slot, "released, it waits in the graveyard" );
		gpu->WaitIdle();
		gpu->graveyard.Collect();
		const uint32 reused = heap.Acquire( Packed( 3 ) );
		Check( reused == slot, va( "after the GPU, another sampler gets it: %u, not %u", reused, slot ) );

		int most = 0;
		for ( int i = 0; i < 3 * static_cast<int>( idD3D12SamplerHeap::SIZE ); i++ ) {
			heap.Release( heap.Acquire( Packed( 100 + i ) ) );
			most = Max( most, heap.Used() );
			if ( i % 500 == 499 ) {
				gpu->WaitIdle();
			}
		}
		Check( most <= 4, va( "%d samplers, at most %d held at once", 3 * static_cast<int>( idD3D12SamplerHeap::SIZE ), most ) );
		gpu->WaitIdle();
		gpu->graveyard.Collect();
	}
	CheckNoLayerErrors( *gpu );
}

// ---- pipeline state: pipelineKey_t → the PSO description ---------------------------------------------------

constexpr uint16 GUI_ATTRIBS = ( 1 << PC_ATTRIB_INDEX_VERTEX ) | ( 1 << PC_ATTRIB_INDEX_ST ) | ( 1 << PC_ATTRIB_INDEX_COLOR );
constexpr d3d12DepthBias_t TEST_BIAS = { -100, -2.0f };

[[nodiscard]] pipelineKey_t Key( uint64 stateBits, int raster = 0, int layout = LAYOUT_DRAW_VERT ) {
	pipelineKey_t key = {};
	key.stateBits = stateBits;
	key.raster = static_cast<uint8>( raster );
	key.layout = static_cast<uint8>( layout );
	return key;
}

[[nodiscard]] d3d12PipelineDesc_t Desc( const pipelineKey_t & key, uint16 attribs = GUI_ATTRIBS, bool depthBounds = false ) {
	return D3D12_PipelineDescFor( D3D12_NormalizeKey( key ), attribs, TEST_BIAS, depthBounds );
}

// Every field of a description, so equal strings are equal PSOs.
[[nodiscard]] std::string DescBytes( const d3d12PipelineDesc_t & desc ) {
	std::string out;
	const auto put = [&out]( const auto value ) { out.append( reinterpret_cast<const char *>( &value ), sizeof( value ) ); };
	put( desc.blend.AlphaToCoverageEnable );
	put( desc.blend.IndependentBlendEnable );
	for ( const D3D12_RENDER_TARGET_BLEND_DESC & target : desc.blend.RenderTarget ) {
		put( target.BlendEnable );
		put( target.LogicOpEnable );
		put( target.SrcBlend );
		put( target.DestBlend );
		put( target.BlendOp );
		put( target.SrcBlendAlpha );
		put( target.DestBlendAlpha );
		put( target.BlendOpAlpha );
		put( target.LogicOp );
		put( target.RenderTargetWriteMask );
	}
	const D3D12_RASTERIZER_DESC & raster = desc.raster;
	put( raster.FillMode );
	put( raster.CullMode );
	put( raster.FrontCounterClockwise );
	put( raster.DepthBias );
	put( raster.DepthBiasClamp );
	put( raster.SlopeScaledDepthBias );
	put( raster.DepthClipEnable );
	put( raster.MultisampleEnable );
	put( raster.AntialiasedLineEnable );
	put( raster.ForcedSampleCount );
	put( raster.ConservativeRaster );
	const D3D12_DEPTH_STENCIL_DESC1 & depth = desc.depthStencil;
	put( depth.DepthEnable );
	put( depth.DepthWriteMask );
	put( depth.DepthFunc );
	put( depth.StencilEnable );
	put( depth.StencilReadMask );
	put( depth.StencilWriteMask );
	for ( const D3D12_DEPTH_STENCILOP_DESC & face : { depth.FrontFace, depth.BackFace } ) {
		put( face.StencilFailOp );
		put( face.StencilDepthFailOp );
		put( face.StencilPassOp );
		put( face.StencilFunc );
	}
	put( depth.DepthBoundsTestEnable );
	put( desc.topology );
	put( desc.colorFormat );
	put( desc.sample.Count );
	put( desc.sample.Quality );
	put( desc.numInputs );
	put( desc.defaultAttribs );
	for ( uint32 i = 0; i < desc.numInputs; i++ ) {
		const D3D12_INPUT_ELEMENT_DESC & input = desc.inputs[i];
		out += input.SemanticName;
		put( input.SemanticIndex );
		put( input.Format );
		put( input.InputSlot );
		put( input.AlignedByteOffset );
		put( input.InputSlotClass );
		put( input.InstanceDataStepRate );
	}
	return out;
}

void Test_PipelineBlend() {
	for ( uint64 src = 0; src < 8; src++ ) {
		for ( uint64 dst = 0; dst < 8; dst++ ) {
			const D3D12_RENDER_TARGET_BLEND_DESC target = Desc( Key( src | ( dst << 3 ) ) ).blend.RenderTarget[0];
			const bool colorFactor = target.SrcBlendAlpha == D3D12_BLEND_SRC_COLOR || target.SrcBlendAlpha == D3D12_BLEND_INV_SRC_COLOR
				|| target.SrcBlendAlpha == D3D12_BLEND_DEST_COLOR || target.SrcBlendAlpha == D3D12_BLEND_INV_DEST_COLOR
				|| target.DestBlendAlpha == D3D12_BLEND_SRC_COLOR || target.DestBlendAlpha == D3D12_BLEND_INV_SRC_COLOR
				|| target.DestBlendAlpha == D3D12_BLEND_DEST_COLOR || target.DestBlendAlpha == D3D12_BLEND_INV_DEST_COLOR;
			Check( !colorFactor, va( "src %llu dst %llu: a *_COLOR alpha factor", src, dst ) );
			Check( ( target.BlendEnable != FALSE ) == ( src != 0 || dst != 0 ), va( "src %llu dst %llu: blending on unless ONE, ZERO", src, dst ) );
			Check( target.BlendOp == D3D12_BLEND_OP_ADD && target.BlendOpAlpha == D3D12_BLEND_OP_ADD && !target.LogicOpEnable, "ADD, no logic op" );
		}
	}
	const D3D12_RENDER_TARGET_BLEND_DESC modulate = Desc( Key( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO ) ).blend.RenderTarget[0];
	Check( modulate.SrcBlend == D3D12_BLEND_DEST_COLOR && modulate.SrcBlendAlpha == D3D12_BLEND_DEST_ALPHA && modulate.DestBlend == D3D12_BLEND_ZERO
		&& modulate.DestBlendAlpha == D3D12_BLEND_ZERO, "DST_COLOR, ZERO: DEST_ALPHA for alpha" );
	const D3D12_RENDER_TARGET_BLEND_DESC inverse = Desc( Key( GLS_SRCBLEND_ONE_MINUS_DST_COLOR | GLS_DSTBLEND_ONE_MINUS_SRC_COLOR ) ).blend.RenderTarget[0];
	Check( inverse.SrcBlend == D3D12_BLEND_INV_DEST_COLOR && inverse.SrcBlendAlpha == D3D12_BLEND_INV_DEST_ALPHA
		&& inverse.DestBlend == D3D12_BLEND_INV_SRC_COLOR && inverse.DestBlendAlpha == D3D12_BLEND_INV_SRC_ALPHA, "the inverse colour factors" );
	const D3D12_RENDER_TARGET_BLEND_DESC color = Desc( Key( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_SRC_COLOR ) ).blend.RenderTarget[0];
	Check( color.SrcBlend == D3D12_BLEND_ZERO && color.DestBlend == D3D12_BLEND_SRC_COLOR && color.DestBlendAlpha == D3D12_BLEND_SRC_ALPHA, "SRC_COLOR" );
	const D3D12_RENDER_TARGET_BLEND_DESC alpha = Desc( Key( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) ).blend.RenderTarget[0];
	Check( alpha.BlendEnable && alpha.SrcBlend == D3D12_BLEND_SRC_ALPHA && alpha.SrcBlendAlpha == D3D12_BLEND_SRC_ALPHA
		&& alpha.DestBlend == D3D12_BLEND_INV_SRC_ALPHA && alpha.DestBlendAlpha == D3D12_BLEND_INV_SRC_ALPHA, "the GUI's alpha blend" );
	const D3D12_RENDER_TARGET_BLEND_DESC opaque = Desc( Key( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO ) ).blend.RenderTarget[0];
	Check( !opaque.BlendEnable && opaque.SrcBlend == D3D12_BLEND_ONE && opaque.DestBlend == D3D12_BLEND_ZERO, "ONE, ZERO: off, as GL disables GL_BLEND" );

	struct mask_t {
		uint64	bits;
		UINT8	writeMask;
	};
	const mask_t masks[] = {
		{ 0, D3D12_COLOR_WRITE_ENABLE_ALL },
		{ GLS_COLORMASK, D3D12_COLOR_WRITE_ENABLE_ALPHA },
		{ GLS_ALPHAMASK, D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE },
		{ GLS_REDMASK | GLS_BLUEMASK, D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_ALPHA },
		{ GLS_COLORMASK | GLS_ALPHAMASK, 0 },
	};
	for ( const mask_t & mask : masks ) {
		const D3D12_BLEND_DESC blend = Desc( Key( mask.bits ) ).blend;
		Check( blend.RenderTarget[0].RenderTargetWriteMask == mask.writeMask, va( "mask bits 0x%llx: write mask 0x%x", mask.bits,
			blend.RenderTarget[0].RenderTargetWriteMask ) );
		Check( !blend.IndependentBlendEnable && !blend.AlphaToCoverageEnable, "one blend for every target, no alpha to coverage" );
	}
}

void Test_PipelineDepth() {
	struct row_t {
		uint64					bits;
		int						raster;
		D3D12_COMPARISON_FUNC	func;
	};
	const row_t rows[] = {
		{ GLS_DEPTHFUNC_LESS, 0, D3D12_COMPARISON_FUNC_LESS_EQUAL },
		{ GLS_DEPTHFUNC_ALWAYS, 0, D3D12_COMPARISON_FUNC_ALWAYS },
		{ GLS_DEPTHFUNC_GREATER, 0, D3D12_COMPARISON_FUNC_GREATER_EQUAL },
		{ GLS_DEPTHFUNC_EQUAL, 0, D3D12_COMPARISON_FUNC_EQUAL },
		{ GLS_DEPTHFUNC_LESS, D3D12_KEY_STRICT_DEPTH, D3D12_COMPARISON_FUNC_LESS },
		{ GLS_DEPTHFUNC_EQUAL, D3D12_KEY_STRICT_DEPTH, D3D12_COMPARISON_FUNC_LESS },	// GL_LESS from SetDefaultState until a State changes it
	};
	for ( const row_t & row : rows ) {
		for ( const uint64 depthMask : { uint64( 0 ), GLS_DEPTHMASK } ) {
			const D3D12_DEPTH_STENCIL_DESC1 depth = Desc( Key( row.bits | depthMask, row.raster ) ).depthStencil;
			Check( depth.DepthEnable && depth.DepthFunc == row.func, va( "depth bits 0x%llx, raster 0x%x: func %d", row.bits, row.raster, depth.DepthFunc ) );
			Check( depth.DepthWriteMask == ( depthMask != 0 ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL ), "GLS_DEPTHMASK stops writes only" );
		}
	}
	Check( D3D12_NormalizeKey( Key( GLS_DEPTHFUNC_EQUAL, D3D12_KEY_STRICT_DEPTH ) ) == D3D12_NormalizeKey( Key( 0, D3D12_KEY_STRICT_DEPTH ) ),
		"under the strict bit the depth function bits do not count" );
	Check( Desc( Key( 0 ), GUI_ATTRIBS, true ).depthStencil.DepthBoundsTestEnable && !Desc( Key( 0 ) ).depthStencil.DepthBoundsTestEnable,
		"depth bounds in every PSO when the device has them" );
}

void Test_PipelineStencil() {
	const D3D12_DEPTH_STENCIL_DESC1 off = Desc( Key( GLS_STENCIL_MAKE_MASK( 0x0F ) | GLS_STENCIL_MAKE_REF( 5 ) ) ).depthStencil;
	Check( !off.StencilEnable, "no function and no front ops: no stencil test" );
	Check( D3D12_NormalizeKey( Key( GLS_STENCIL_MAKE_MASK( 0x0F ) | GLS_SEPARATE_STENCIL ) ) == D3D12_NormalizeKey( Key( 0 ) ),
		"with the test off the mask and the back ops do not count" );
	// GLS:316-322 reads the front ops only: back ops alone leave GL's test off
	const D3D12_DEPTH_STENCIL_DESC1 backOnly = Desc( Key( GLS_SEPARATE_STENCIL | GLS_BackStencilOps( GLS_STENCIL_OP_PASS_INCR ) ) ).depthStencil;
	Check( !backOnly.StencilEnable, "back ops alone: no stencil test, as in GL" );

	const D3D12_DEPTH_STENCIL_DESC1 swf = Desc( Key( GLS_STENCIL_FUNC_EQUAL | GLS_STENCIL_MAKE_REF( 129 ) | GLS_STENCIL_MAKE_MASK( 255 ) ) ).depthStencil;
	Check( swf.StencilEnable && swf.StencilReadMask == 255 && swf.StencilWriteMask == 255 && swf.FrontFace.StencilFunc == D3D12_COMPARISON_FUNC_EQUAL
		&& swf.FrontFace.StencilPassOp == D3D12_STENCIL_OP_KEEP && swf.BackFace.StencilFunc == D3D12_COMPARISON_FUNC_EQUAL, "a function alone turns the test on" );
	const D3D12_DEPTH_STENCIL_DESC1 incr = Desc( Key( GLS_STENCIL_OP_PASS_INCR ) ).depthStencil;
	Check( incr.StencilEnable && incr.FrontFace.StencilFunc == D3D12_COMPARISON_FUNC_ALWAYS && incr.FrontFace.StencilPassOp == D3D12_STENCIL_OP_INCR_SAT
		&& incr.BackFace.StencilPassOp == D3D12_STENCIL_OP_INCR_SAT, "front ops alone turn the test on; GL's INCR saturates" );
	const D3D12_DEPTH_STENCILOP_DESC wrap = Desc( Key( GLS_STENCIL_OP_FAIL_INCR_WRAP | GLS_STENCIL_OP_ZFAIL_DECR_WRAP | GLS_STENCIL_OP_PASS_DECR ) ).depthStencil.FrontFace;
	Check( wrap.StencilFailOp == D3D12_STENCIL_OP_INCR && wrap.StencilDepthFailOp == D3D12_STENCIL_OP_DECR && wrap.StencilPassOp == D3D12_STENCIL_OP_DECR_SAT,
		"INCR_WRAP, DECR_WRAP, DECR: INCR, DECR, DECR_SAT" );
	const D3D12_DEPTH_STENCILOP_DESC others = Desc( Key( GLS_STENCIL_OP_FAIL_ZERO | GLS_STENCIL_OP_ZFAIL_REPLACE | GLS_STENCIL_OP_PASS_INVERT ) ).depthStencil.FrontFace;
	Check( others.StencilFailOp == D3D12_STENCIL_OP_ZERO && others.StencilDepthFailOp == D3D12_STENCIL_OP_REPLACE && others.StencilPassOp == D3D12_STENCIL_OP_INVERT,
		"ZERO, REPLACE, INVERT" );
	const D3D12_COMPARISON_FUNC funcs[8] = { D3D12_COMPARISON_FUNC_ALWAYS, D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_COMPARISON_FUNC_GREATER, D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_EQUAL, D3D12_COMPARISON_FUNC_NOT_EQUAL,
		D3D12_COMPARISON_FUNC_NEVER };
	for ( uint64 func = 1; func < 8; func++ ) {
		const D3D12_DEPTH_STENCIL_DESC1 depth = Desc( Key( ( func << 36 ) | GLS_STENCIL_MAKE_MASK( 0x3C ) ) ).depthStencil;
		Check( depth.FrontFace.StencilFunc == funcs[func] && depth.BackFace.StencilFunc == funcs[func] && depth.StencilReadMask == 0x3C,
			va( "stencil func %llu", func ) );
	}

	struct separate_t {
		const char *		name;
		uint64				bits;
		D3D12_STENCIL_OP	front[3];
		D3D12_STENCIL_OP	back[3];
	};
	const separate_t separates[] = {
		{ "Z-pass", GLS_STENCIL_SHADOW_ZPASS, { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_INCR_SAT },
			{ D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_DECR_SAT } },
		{ "preload", GLS_STENCIL_SHADOW_PRELOAD, { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_DECR_SAT, D3D12_STENCIL_OP_DECR_SAT },
			{ D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_INCR_SAT, D3D12_STENCIL_OP_INCR_SAT } },
		{ "light select", GLS_STENCIL_LIGHT_SELECT, { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_ZERO },
			{ D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_REPLACE } },
	};
	for ( const separate_t & row : separates ) {
		const D3D12_DEPTH_STENCIL_DESC1 depth = Desc( Key( row.bits | GLS_STENCIL_FUNC_ALWAYS | GLS_STENCIL_MAKE_MASK( 255 ) ) ).depthStencil;
		const D3D12_DEPTH_STENCILOP_DESC & front = depth.FrontFace;
		const D3D12_DEPTH_STENCILOP_DESC & back = depth.BackFace;
		Check( depth.StencilEnable && front.StencilFailOp == row.front[0] && front.StencilDepthFailOp == row.front[1] && front.StencilPassOp == row.front[2],
			va( "%s: front", row.name ) );
		Check( back.StencilFailOp == row.back[0] && back.StencilDepthFailOp == row.back[1] && back.StencilPassOp == row.back[2], va( "%s: back", row.name ) );
	}
	const uint64 front = GLS_STENCIL_OP_ZFAIL_INCR | GLS_STENCIL_OP_PASS_ZERO;
	Check( D3D12_NormalizeKey( Key( front | GLS_BackStencilOps( GLS_STENCIL_OP_PASS_DECR ) ) ) == D3D12_NormalizeKey( Key( front ) ),
		"without SEPARATE_STENCIL the back ops do not count" );
	Check( D3D12_NormalizeKey( Key( front | GLS_SEPARATE_STENCIL | GLS_BackStencilOps( front ) ) ) == D3D12_NormalizeKey( Key( front ) ),
		"separate ops equal to the front ones are no separate ops" );
	Check( D3D12_NormalizeKey( Key( GLS_STENCIL_FUNC_LESS | GLS_STENCIL_MAKE_REF( 1 ) ) ) == D3D12_NormalizeKey( Key( GLS_STENCIL_FUNC_LESS | GLS_STENCIL_MAKE_REF( 200 ) ) ),
		"the reference is dynamic" );
}

void Test_PipelineRaster() {
	Check( R_PipelineCull( CT_FRONT_SIDED, true, false ) == 1 && R_PipelineCull( CT_BACK_SIDED, true, false ) == 2, "front-sided culls GL_FRONT, back-sided GL_BACK" );
	Check( R_PipelineCull( CT_FRONT_SIDED, true, true ) == 2 && R_PipelineCull( CT_BACK_SIDED, true, true ) == 1, "a mirror swaps them" );
	Check( R_PipelineCull( CT_TWO_SIDED, true, false ) == 0 && R_PipelineCull( CT_FRONT_SIDED, false, true ) == 0, "two-sided, or culling still off" );
	const D3D12_CULL_MODE modes[3] = { D3D12_CULL_MODE_NONE, D3D12_CULL_MODE_FRONT, D3D12_CULL_MODE_BACK };
	for ( int cull = 0; cull < 3; cull++ ) {
		const D3D12_RASTERIZER_DESC raster = Desc( Key( 0, cull ) ).raster;
		Check( raster.CullMode == modes[cull] && !raster.FrontCounterClockwise, va( "raster cull %d: CullMode %d, clockwise front faces", cull, raster.CullMode ) );
		Check( Desc( Key( 0, cull | D3D12_KEY_LINES ) ).raster.CullMode == D3D12_CULL_MODE_NONE, "lines are not culled" );
	}
	Check( D3D12_NormalizeKey( Key( 0, 1 | D3D12_KEY_LINES ) ) == D3D12_NormalizeKey( Key( 0, D3D12_KEY_LINES ) ), "a line key has no cull" );

	const d3d12PipelineDesc_t wire = Desc( Key( GLS_POLYMODE_LINE ) );
	Check( wire.raster.FillMode == D3D12_FILL_MODE_WIREFRAME && wire.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, "POLYMODE_LINE: wireframe triangles" );
	const d3d12PipelineDesc_t lines = Desc( Key( GLS_POLYMODE_LINE, D3D12_KEY_LINES ) );
	Check( lines.raster.FillMode == D3D12_FILL_MODE_SOLID && lines.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, "lines: LINE topology" );

	pipelineKey_t biased = Key( GLS_POLYGON_OFFSET );
	biased.depthBias = 1;
	const D3D12_RASTERIZER_DESC offset = Desc( biased ).raster;
	Check( offset.DepthBias == TEST_BIAS.bias && offset.SlopeScaledDepthBias == TEST_BIAS.slope && offset.DepthBiasClamp == 0.0f, "the slot's bias" );
	Check( Desc( Key( GLS_POLYGON_OFFSET ) ).raster.DepthBias == 0, "slot 0: no bias, whatever the GLS bit" );
	biased.raster = D3D12_KEY_LINES;
	Check( D3D12_NormalizeKey( biased ).depthBias == 0 && Desc( biased ).raster.DepthBias == 0 && Desc( biased ).raster.SlopeScaledDepthBias == 0.0f,
		"no depth bias on lines: GL offsets polygons only" );
	Check( D3D12_DepthBiasFor( -2.0f, -100.4f ) == d3d12DepthBias_t{ -100, -2.0f } && D3D12_DepthBiasFor( 1.5f, 2.5f ).bias == 3, "units round to an INT" );

	const d3d12PipelineDesc_t plain = Desc( Key( 0, d3d12TargetId_t::Of( false, 1 ).value << D3D12_KEY_TARGET_SHIFT ) );
	Check( plain.colorFormat == DXGI_FORMAT_R8G8B8A8_UNORM && plain.sample.Count == 1 && plain.sample.Quality == 0, "target 0: UNORM, one sample" );
	const d3d12PipelineDesc_t srgb = Desc( Key( 0, d3d12TargetId_t::Of( true, 4 ).value << D3D12_KEY_TARGET_SHIFT ) );
	Check( srgb.colorFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && srgb.sample.Count == 4, "the target id: the sRGB view, 4 samples" );
	Check( plain.raster.DepthClipEnable && !plain.raster.MultisampleEnable && !plain.raster.AntialiasedLineEnable, "depth clip, aliased lines" );
}

void Test_PipelineInputs() {
	const d3d12PipelineDesc_t gui = Desc( Key( 0 ) );
	Check( gui.numInputs == 3 && !gui.defaultAttribs, "gui: three inputs from the draw vert" );
	if ( gui.numInputs == 3 ) {
		const D3D12_INPUT_ELEMENT_DESC * inputs = gui.inputs.data();
		Check( idStr::Cmp( inputs[0].SemanticName, "POSITION" ) == 0 && inputs[0].Format == DXGI_FORMAT_R32G32B32_FLOAT && inputs[0].AlignedByteOffset == DRAWVERT_XYZ_OFFSET
			&& inputs[0].InputSlot == 0, "POSITION" );
		Check( idStr::Cmp( inputs[1].SemanticName, "TEXCOORD" ) == 0 && inputs[1].SemanticIndex == 0 && inputs[1].Format == DXGI_FORMAT_R16G16_FLOAT
			&& inputs[1].AlignedByteOffset == DRAWVERT_ST_OFFSET, "TEXCOORD0: two halves" );
		Check( idStr::Cmp( inputs[2].SemanticName, "COLOR" ) == 0 && inputs[2].SemanticIndex == 0 && inputs[2].Format == DXGI_FORMAT_R8G8B8A8_UNORM
			&& inputs[2].AlignedByteOffset == DRAWVERT_COLOR_OFFSET && inputs[2].InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, "COLOR0" );
	}
	Check( Desc( Key( 0 ), static_cast<uint16>( vertexLayouts[LAYOUT_DRAW_VERT].attribMask ) ).numInputs == 6, "all six of a draw vert" );

	const uint16 positionColor = ( 1 << PC_ATTRIB_INDEX_VERTEX ) | ( 1 << PC_ATTRIB_INDEX_COLOR );
	const d3d12PipelineDesc_t shadow = Desc( Key( 0, 0, LAYOUT_DRAW_SHADOW_VERT ), positionColor );
	Check( shadow.numInputs == 2 && shadow.defaultAttribs, "a shadow vert lacks the colour" );
	if ( shadow.numInputs == 2 ) {
		Check( shadow.inputs[0].Format == DXGI_FORMAT_R32G32B32A32_FLOAT && shadow.inputs[0].InputSlot == 0, "the shadow vert's xyzw" );
		Check( shadow.inputs[1].InputSlot == 1 && shadow.inputs[1].AlignedByteOffset == 0 && shadow.inputs[1].Format == DXGI_FORMAT_R32G32B32A32_FLOAT,
			"the missing colour reads (0, 0, 0, 1) from slot 1" );
	}
	const uint16 skinnedAttribs = positionColor | ( 1 << PC_ATTRIB_INDEX_COLOR2 );
	const d3d12PipelineDesc_t skinned = Desc( Key( 0, 0, LAYOUT_DRAW_SHADOW_VERT_SKINNED ), skinnedAttribs );
	Check( skinned.numInputs == 3 && !skinned.defaultAttribs && skinned.inputs[2].SemanticIndex == 1
		&& skinned.inputs[2].AlignedByteOffset == SHADOWVERTSKINNED_COLOR2_OFFSET, "the skinned shadow vert: COLOR1 at its offset" );
}

// What a PSO of the key is: its program, its layout (the IA's stride) and its description.
[[nodiscard]] std::string PipelineBytes( const pipelineKey_t & key ) {
	const uint16 program = key.program;
	const uint16 attribs = static_cast<uint16>( ( 1 << PC_ATTRIB_INDEX_VERTEX ) | ( ( program & 1 ) ? 1 << PC_ATTRIB_INDEX_ST : 0 )
		| ( ( program & 2 ) ? 1 << PC_ATTRIB_INDEX_COLOR : 0 ) | ( ( program & 4 ) ? 1 << PC_ATTRIB_INDEX_NORMAL : 0 )
		| ( ( program & 8 ) ? 1 << PC_ATTRIB_INDEX_COLOR2 : 0 ) | ( ( program & 16 ) ? 1 << PC_ATTRIB_INDEX_TANGENT : 0 ) );
	const d3d12DepthBias_t bias = ( key.depthBias == 0 ) ? d3d12DepthBias_t() : d3d12DepthBias_t{ key.depthBias, key.depthBias * 0.5f };
	const uint8 layout = ( key.layout < NUM_VERTEX_LAYOUTS ) ? key.layout : static_cast<uint8>( LAYOUT_UNKNOWN );
	std::string bytes( reinterpret_cast<const char *>( &program ), sizeof( program ) );
	bytes.push_back( static_cast<char>( layout ) );
	return bytes + DescBytes( D3D12_PipelineDescFor( key, attribs, bias, true ) );
}

class idTestRandom {
public:
	explicit idTestRandom( uint64 seed ) : state( seed ) {}
	uint32 Next() {
		state = state * 6364136223846793005ull + 1442695040888963407ull;
		return static_cast<uint32>( state >> 33 );
	}
	uint64 Bits( int count ) { return ( ( static_cast<uint64>( Next() ) << 32 ) | Next() ) & ( ( count == 64 ) ? ~0ull : ( 1ull << count ) - 1 ); }
	bool OneIn( uint32 n ) { return Next() % n == 0; }
private:
	uint64 state;
};

// Mostly the values shared code sends, sometimes anything: the stencil test on and off, separate ops equal to the front ones.
[[nodiscard]] pipelineKey_t RandomKey( idTestRandom & random ) {
	uint64 bits = random.Bits( 15 );		// blend, masks, polygon mode and offset, depth function
	bits |= random.OneIn( 4 ) ? random.Bits( 5 ) << 15 : 0;
	bits |= random.Bits( 8 ) << GLS_STENCIL_FUNC_REF_SHIFT;
	bits |= ( random.OneIn( 2 ) ? 0xFFull : random.Bits( 8 ) ) << GLS_STENCIL_FUNC_MASK_SHIFT;
	if ( !random.OneIn( 3 ) ) {
		bits |= ( random.OneIn( 3 ) ? 0 : random.Bits( 3 ) ) << 36;
		for ( int op = 0; op < 3; op++ ) {
			bits |= ( random.OneIn( 2 ) ? 0 : random.Bits( 3 ) ) << ( 39 + op * 3 );
		}
	}
	bits |= ( random.OneIn( 3 ) ? GLS_BackStencilOps( bits ) : random.Bits( 9 ) << 48 ) | ( random.OneIn( 2 ) ? GLS_SEPARATE_STENCIL : 0 );
	bits |= random.OneIn( 4 ) ? random.Bits( 6 ) << 58 : 0;
	pipelineKey_t key = {};
	key.stateBits = bits;
	key.program = static_cast<uint16>( random.Bits( 5 ) );
	key.layout = static_cast<uint8>( random.OneIn( 8 ) ? random.Bits( 8 ) : 1 + random.Next() % 3 );
	key.raster = static_cast<uint8>( random.Bits( random.OneIn( 4 ) ? 8 : 7 ) );
	key.depthBias = static_cast<uint8>( random.OneIn( 2 ) ? 0 : random.OneIn( 4 ) ? random.Bits( 8 ) : 1 + random.Next() % 2 );
	if ( random.OneIn( 4 ) ) {
		key.pad[random.Next() % 3] = static_cast<uint8>( random.Bits( 8 ) );
	}
	return key;
}

// Equal PSO descriptions ⇔ equal normalized keys: every single-bit change of random keys, and field changes that
// make the back ops equal to the front ones or turn the stencil test off.
void Test_PipelineNormalize() {
	idTestRandom random( 0x5EED );
	int compared = 0;
	int samePipeline = 0;
	for ( int i = 0; i < 2000; i++ ) {
		const pipelineKey_t key = RandomKey( random );
		const pipelineKey_t normal = D3D12_NormalizeKey( key );
		const std::string bytes = PipelineBytes( key );
		Check( D3D12_NormalizeKey( normal ) == normal, "normalizing twice changes nothing" );
		Check( PipelineBytes( normal ) == bytes, va( "key %d: normalizing changed the PSO", i ) );
		std::vector<pipelineKey_t> variants;
		for ( int bit = 0; bit < 128; bit++ ) {
			pipelineKey_t flipped = key;
			reinterpret_cast<byte *>( &flipped )[bit / 8] ^= static_cast<byte>( 1 << ( bit % 8 ) );
			variants.push_back( flipped );
		}
		pipelineKey_t copied = key;
		copied.stateBits = ( copied.stateBits & ~GLS_STENCIL_BACK_OP_BITS ) | GLS_BackStencilOps( copied.stateBits );
		variants.push_back( copied );
		pipelineKey_t stencilOff = key;
		stencilOff.stateBits &= ~( GLS_STENCIL_FUNC_BITS | GLS_STENCIL_FRONT_OP_BITS );
		variants.push_back( stencilOff );
		for ( const pipelineKey_t & variant : variants ) {
			const bool sameKey = ( D3D12_NormalizeKey( variant ) == normal );
			const bool sameBytes = ( PipelineBytes( variant ) == bytes );
			if ( !Check( sameKey == sameBytes, va( "key %d: equal normalized keys %d, equal PSOs %d (0x%016llx %04x %02x %02x %02x vs 0x%016llx %02x)", i, sameKey,
					sameBytes, key.stateBits, key.program, key.layout, key.raster, key.depthBias, variant.stateBits, variant.raster ) ) ) {
				continue;
			}
			compared++;
			samePipeline += sameBytes ? 1 : 0;
		}
	}
	Check( samePipeline > compared / 10 && samePipeline < compared - compared / 10, va( "%d of %d variants the same PSO: both sides tested", samePipeline, compared ) );
}

void Test_DepthBiasTable() {
	idD3D12DepthBiasTable table;
	Check( table.Slot( 0.0f, 0.0f ) == 0 && table.Slot( 0.0f, 0.3f ) == 0 && table.Num() == 0, "no bias, no slot" );
	const uint8 first = table.Slot( -2.0f, -100.0f );
	Check( first == 1 && table[first] == d3d12DepthBias_t{ -100, -2.0f }, "the first bias takes slot 1" );
	Check( table.Slot( -2.0f, -100.4f ) == first, "units that round to the same INT share the slot" );
	const uint8 second = table.Slot( -1.0f, -100.0f );
	Check( second == 2 && table.Slot( -2.0f, -100.0f ) == first && table.Num() == 2, "another factor, another slot; the first is found again" );
	Check( table[0] == d3d12DepthBias_t(), "slot 0 is no bias" );
	Check( table.SlotOf( d3d12DepthBias_t{ -100, -1.0f } ) == second && table.SlotOf( d3d12DepthBias_t() ) == 0 && table.Num() == 2,
		"a journal's bias finds its slot by value" );
	Check( table.SlotOf( d3d12DepthBias_t{ -7, 0.5f } ) == 3 && table.Slot( 0.5f, -7.0f ) == 3 && table.Num() == 3,
		"a journal's new bias takes a slot, and the draw that sets it finds it" );
	for ( int i = 4; i <= 255; i++ ) {
		static_cast<void>( table.Slot( static_cast<float>( i ), 0.0f ) );
	}
	Check( table.Num() == 255 && table[255] == d3d12DepthBias_t{ 0, 255.0f }, "255 slots" );
	Check( table.NearestThisFrame() == 0, "nothing reused while the table has room" );

	Check( table.Slot( 1000.0f, 0.0f ) == 255 && table.Slot( 1000.0f, 0.0f ) == 255 && table.Num() == 255,
		"a full table gives the nearest bias, also to the cached pair" );
	Check( table.Slot( 2.4f, -99.0f ) == 2, "the nearest by factor and units: slot 2 (-1, -100)" );
	Check( table.Slot( 0.0f, -1.0f ) == 0, "no bias is a candidate too" );
	Check( table.Slot( -2.0f, -100.0f ) == first, "a bias the table has is still exact" );
	Check( table.SlotOf( d3d12DepthBias_t{ 0, 1000.0f } ) == 255, "a full table gives a journal's bias the nearest one too" );
	Check( table.NearestThisFrame() == 4, va( "each draw with a nearest bias counts, a journal's bias does not: %d of 4", table.NearestThisFrame() ) );
	table.ResetFrameCounters();
	Check( table.NearestThisFrame() == 0, "the count is per frame" );
}

void Test_PipelineJournal() {
	idD3D12PipelineJournal journal;
	Check( !journal.Dirty() && journal.NumEntries() == 0 && journal.EntriesFor( "gui|gui" ).empty(), "an empty journal" );
	d3d12JournalEntry_t a;
	a.key.stateBits = GLS_DEPTHFUNC_EQUAL | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
	a.key.layout = LAYOUT_DRAW_VERT;
	a.key.raster = 2;
	d3d12JournalEntry_t b = a;
	b.bias = d3d12DepthBias_t{ -100, -2.0f };
	d3d12JournalEntry_t c = a;
	c.key.layout = LAYOUT_DRAW_SHADOW_VERT;
	c.key.raster = D3D12_KEY_LINES | ( d3d12TargetId_t::Of( true, 4 ).value << D3D12_KEY_TARGET_SHIFT );
	journal.Record( "interaction|interaction", a );
	journal.Record( "interaction|interaction", b );
	journal.Record( "shadow|-", c );
	journal.Record( "interaction|interaction", a );
	Check( journal.Dirty() && journal.NumPrograms() == 2 && journal.NumEntries() == 3, "a known key is ignored" );
	const std::span<const d3d12JournalEntry_t> entries = journal.EntriesFor( "interaction|interaction" );
	Check( entries.size() == 2 && entries[0] == a && entries[1] == b, "the keys of a program, in the order drawn" );
	journal.MarkSaved();
	journal.Record( "shadow|-", c );
	Check( !journal.Dirty(), "a known key leaves a saved journal clean" );

	const std::vector<byte> file = journal.Write();
	idD3D12PipelineJournal read;
	Check( read.Read( file ) && !read.Dirty() && read.NumPrograms() == 2 && read.NumEntries() == 3 && read.Write() == file,
		"Write, Read and Write again: the same file" );
	Check( read.EntriesFor( "shadow|-" ).size() == 1 && read.EntriesFor( "shadow|-" )[0] == c, "the keys come back" );
	idD3D12PipelineJournal reordered;
	reordered.Record( "shadow|-", c );
	reordered.Record( "interaction|interaction", a );
	reordered.Record( "interaction|interaction", b );
	Check( reordered.Write() == file, "sorted by program: the same keys write the same file" );

	std::vector<byte> stale = file;
	stale[4] ^= 0xFF;
	Check( !read.Read( stale ) && read.NumEntries() == 0 && read.EntriesFor( "shadow|-" ).empty(), "another VERSION is not read" );
	Check( !read.Read( std::span( file ).first( file.size() - 1 ) ) && read.NumPrograms() == 0, "a file cut short is not read" );
	std::vector<byte> longer = file;
	longer.push_back( 0 );
	Check( !read.Read( longer ), "nor one with bytes after its end" );
	Check( !read.Read( {} ), "nor no file" );
	idD3D12PipelineJournal foreign;
	d3d12JournalEntry_t bad = a;
	bad.key.layout = NUM_VERTEX_LAYOUTS;
	foreign.Record( "gui|gui", bad );
	Check( !read.Read( foreign.Write() ), "a layout the backend has not: not a journal this code wrote" );

	idD3D12PipelineJournal hdr;
	d3d12JournalEntry_t hdrEntry = a;
	hdrEntry.key.raster = static_cast<uint8>( D3D12_KEY_STRICT_DEPTH | D3D12_KeyRaster( d3d12TargetId_t::Of( false, 4, true ) ) );
	hdr.Record( "interaction|interaction", hdrEntry );
	Check( read.Read( hdr.Write() ) && read.EntriesFor( "interaction|interaction" ).size() == 1
		&& read.EntriesFor( "interaction|interaction" )[0] == hdrEntry, "the HDR scene's bit 7 is read back" );
}

// Raster bits 3-5 and 7: every target id goes through the key and back; the HDR scene has no sRGB view.
void Test_PipelineTargetIds() {
	for ( uint8 value = 0; value < d3d12TargetId_t::COUNT; value++ ) {
		const d3d12TargetId_t id = { value };
		Check( D3D12_KeyTarget( D3D12_KeyRaster( id ) ) == id, va( "target id %u round trip", value ) );
		Check( ( D3D12_KeyRaster( id ) & ~( D3D12_KEY_TARGET_BITS | D3D12_KEY_HDR_TARGET ) ) == 0, va( "target id %u stays in its bits", value ) );
	}
	for ( UINT samples : { 1u, 2u, 4u, 8u } ) {
		const d3d12TargetId_t hdr = d3d12TargetId_t::Of( true, samples, true );
		Check( hdr.Hdr() && !hdr.Srgb() && hdr.Samples() == samples && hdr.ColorView() == DXGI_FORMAT_R16G16B16A16_FLOAT,
			va( "the HDR scene of %u samples: RGBA16F, no sRGB view", samples ) );
		const d3d12TargetId_t ldr = d3d12TargetId_t::Of( true, samples );
		Check( !ldr.Hdr() && ldr.Srgb() && ldr.Samples() == samples && ldr.ColorView() == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			va( "the output of %u samples keeps its ids", samples ) );
	}
	pipelineKey_t key = {};
	key.raster = static_cast<uint8>( D3D12_KeyRaster( d3d12TargetId_t::Of( false, 2, true ) ) | ( 1 << D3D12_KEY_TARGET_SHIFT ) );
	Check( D3D12_KeyTarget( D3D12_NormalizeKey( key ).raster ) == d3d12TargetId_t::Of( false, 2, true ), "the normalized key drops an sRGB bit under HDR" );
}

// gui and shadow.vertex alone (no PS), from the repo's base/shaders.
struct testPrograms_t {
	d3d12Program_t					gui;
	d3d12Program_t					shadow;
};

[[nodiscard]] std::optional<testPrograms_t> LinkTestPrograms( idD3D12HlslCompiler & compiler ) {
	const auto guiVertex = RepoStage( compiler, "gui.vertex", shaderStageType_t::VERTEX );
	const auto guiPixel = RepoStage( compiler, "gui.pixel", shaderStageType_t::FRAGMENT );
	const auto shadowVertex = RepoStage( compiler, "shadow.vertex", shaderStageType_t::VERTEX );
	if ( !Check( guiVertex && guiPixel && shadowVertex, "the stages compile" ) ) {
		return std::nullopt;
	}
	auto gui = D3D12_LinkProgram( *guiVertex, &*guiPixel );
	auto shadow = D3D12_LinkProgram( *shadowVertex, nullptr );
	if ( !Check( gui && shadow, "the programs link" ) ) {
		return std::nullopt;
	}
	return testPrograms_t{ std::move( *gui ), std::move( *shadow ) };
}

[[nodiscard]] UINT KeySamples( const pipelineKey_t & key ) {
	return d3d12TargetId_t{ static_cast<uint8>( ( key.raster & D3D12_KEY_TARGET_BITS ) >> D3D12_KEY_TARGET_SHIFT ) }.Samples();
}

// Every family of keys, the line key with the bias of slot, and a key per sample count the adapter has.
[[nodiscard]] std::vector<pipelineKey_t> TestKeys( const idTestGpu & gpu, uint8 slot ) {
	std::vector<pipelineKey_t> keys = {
		Key( 0, D3D12_KEY_STRICT_DEPTH ),
		Key( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS, 0 ),
		Key( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE_MINUS_SRC_COLOR | GLS_COLORMASK, 1 ),
		Key( GLS_SRCBLEND_ONE_MINUS_DST_COLOR | GLS_DSTBLEND_SRC_COLOR | GLS_ALPHAMASK | GLS_DEPTHFUNC_EQUAL, 2 ),
		Key( GLS_STENCIL_FUNC_EQUAL | GLS_STENCIL_MAKE_MASK( 255 ) | GLS_DEPTHFUNC_GREATER, 2 ),
		Key( GLS_STENCIL_OP_PASS_INCR_WRAP | GLS_STENCIL_OP_FAIL_DECR_WRAP | GLS_STENCIL_OP_ZFAIL_INVERT, 1 ),
		Key( GLS_STENCIL_SHADOW_ZPASS | GLS_STENCIL_FUNC_ALWAYS | GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHMASK, 0 ),
		Key( GLS_POLYMODE_LINE | GLS_POLYGON_OFFSET, 2 ),
		Key( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE, D3D12_KEY_LINES ),
	};
	keys[7].depthBias = slot;
	for ( int samples = 1, index = 0; samples <= 8; samples *= 2, index++ ) {
		if ( gpu.device.Features().samples[static_cast<size_t>( index )] ) {
			keys.push_back( Key( 0, d3d12TargetId_t::Of( true, static_cast<UINT>( samples ) ).value << D3D12_KEY_TARGET_SHIFT ) );
		}
	}
	return keys;
}

// Each key with gui and, on the shadow layout, with shadow; the PSOs drawn with.
int DrawTestKeys( idD3D12PipelineFactory & factory, testPrograms_t & programs, std::span<const pipelineKey_t> keys ) {
	int pipelines = 0;
	for ( pipelineKey_t key : keys ) {
		key.program = programs.gui.dense;
		Check( factory.Get( programs.gui, D3D12_NormalizeKey( key ) ) != nullptr, va( "gui: key 0x%016llx raster 0x%02x", key.stateBits, key.raster ) );
		pipelines++;
		pipelineKey_t shadowKey = key;
		shadowKey.layout = LAYOUT_DRAW_SHADOW_VERT;
		Check( factory.Get( programs.shadow, D3D12_NormalizeKey( shadowKey ) ) != nullptr, va( "shadow: key 0x%016llx", key.stateBits ) );
		pipelines++;
	}
	return pipelines;
}

// Against WARP's runtime and debug layer: every family of keys.
void Test_WarpPipelines() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 );
	if ( !gpu ) {
		return;
	}
	idD3D12HlslCompiler compiler( gpu->library, RepoReader(), false );
	std::optional<testPrograms_t> programs = LinkTestPrograms( compiler );
	if ( !programs ) {
		return;
	}
	idD3D12RootSignature root( gpu->device, gpu->library );
	idD3D12PipelineFactory factory( gpu->device, root, nullptr, nullptr );
	const std::vector<pipelineKey_t> keys = TestKeys( *gpu, factory.Bias().Slot( -2.0f, -100.0f ) );
	const int pipelines = DrawTestKeys( factory, *programs, keys );
	Check( factory.CreatedThisFrame() == pipelines && static_cast<int>( programs->gui.pipelines.size() + programs->shadow.pipelines.size() ) == pipelines,
		"one PSO per key" );
	const pipelineKey_t again = D3D12_NormalizeKey( keys[1] );
	Check( factory.Get( programs->gui, again ) == programs->gui.pipelines[1].pso.Get() && factory.CreatedThisFrame() == pipelines,
		"a kept key is not created again" );
	Check( factory.TotalStats().drawn == pipelines && factory.TotalStats().warmed == 0 && factory.TotalStats().loaded == 0, "no journal, no library" );
	CheckNoLayerErrors( *gpu );
}

// A run draws and records; the next one reads the journal and the library back, its bias slots in another
// order, and warms every key of its sample count from the library, so its draws create none of them.
void PipelineWarm( idTestGpu & gpu ) {
	idD3D12HlslCompiler compiler( gpu.library, RepoReader(), false );
	idD3D12RootSignature root( gpu.device, gpu.library );
	std::optional<idD3D12PipelineLibrary> firstLibrary = idD3D12PipelineLibrary::Open( gpu.device, {} );
	if ( !firstLibrary ) {
		printf( "  the adapter keeps no pipeline library: the journal alone\n" );
	}
	Check( !firstLibrary || ( !firstLibrary->FromFile() && !firstLibrary->Dirty() ), "no file: a new, clean library" );
	std::vector<byte> journalFile;
	std::vector<byte> libraryFile;
	int pipelines = 0;
	int ofOneSample = 0;
	{
		std::optional<testPrograms_t> programs = LinkTestPrograms( compiler );
		if ( !programs ) {
			return;
		}
		idD3D12PipelineJournal journal;
		idD3D12PipelineFactory factory( gpu.device, root, &journal, firstLibrary ? &*firstLibrary : nullptr );
		const std::vector<pipelineKey_t> keys = TestKeys( gpu, factory.Bias().Slot( -2.0f, -100.0f ) );
		pipelines = DrawTestKeys( factory, *programs, keys );
		ofOneSample = 2 * static_cast<int>( std::ranges::count_if( keys, []( const pipelineKey_t & key ) { return KeySamples( key ) == 1; } ) );
		Check( journal.Dirty() && journal.NumPrograms() == 2 && journal.NumEntries() == pipelines, "every key drawn is in the journal" );
		Check( !firstLibrary || firstLibrary->Dirty(), "and every PSO in the library" );
		journalFile = journal.Write();
		if ( firstLibrary ) {
			libraryFile = firstLibrary->Write();
			Check( !libraryFile.empty(), "the library serializes" );
		}
	}

	std::optional<testPrograms_t> programs = LinkTestPrograms( compiler );
	if ( !programs ) {
		return;
	}
	idD3D12PipelineJournal journal;
	Check( journal.Read( journalFile ) && !journal.Dirty(), "the next run reads the journal" );
	std::optional<idD3D12PipelineLibrary> library = firstLibrary ? idD3D12PipelineLibrary::Open( gpu.device, libraryFile ) : std::nullopt;
	Check( !firstLibrary || ( library && library->FromFile() && !library->Dirty() ), "and opens the library over its file" );
	idD3D12PipelineFactory factory( gpu.device, root, &journal, library ? &*library : nullptr );
	Check( factory.Bias().Slot( 3.0f, 30.0f ) == 1, "another bias takes slot 1 first" );
	factory.Warm( programs->gui, 1 );
	factory.Warm( programs->shadow, 1 );
	const idD3D12PipelineFactory::stats_t warm = factory.TotalStats();
	Check( warm.warmed == ofOneSample && warm.drawn == 0 && warm.loaded == ( library ? ofOneSample : 0 ) && factory.CreatedThisFrame() == 0,
		va( "the journal's %d keys of one sample warmed (%d), %d from the library", ofOneSample, warm.warmed, warm.loaded ) );
	Check( factory.Bias().Num() == 2 && factory.Bias().SlotOf( d3d12DepthBias_t{ -100, -2.0f } ) == 2, "the journal's bias took slot 2" );
	factory.Warm( programs->gui, 1 );
	Check( factory.TotalStats().warmed == ofOneSample, "a warmed key is not made again" );

	const std::vector<pipelineKey_t> keys = TestKeys( gpu, factory.Bias().Slot( -2.0f, -100.0f ) );
	Check( DrawTestKeys( factory, *programs, keys ) == pipelines, "the same draws" );
	Check( factory.CreatedThisFrame() == pipelines - ofOneSample, va( "only the keys of other sample counts are made by draws: %d of %d",
		factory.CreatedThisFrame(), pipelines - ofOneSample ) );
	Check( factory.TotalStats().loaded == ( library ? pipelines : 0 ), "and those come from the library too" );
	Check( !journal.Dirty() && ( !library || !library->Dirty() ), "nothing new for the journal or the library" );

	if ( library ) {
		std::vector<byte> stale = libraryFile;
		stale[8] ^= 1;	// the driver version
		const std::optional<idD3D12PipelineLibrary> other = idD3D12PipelineLibrary::Open( gpu.device, stale );
		Check( other && !other->FromFile() && other->Dirty(), "another driver's file: an empty library, written again" );
		const std::optional<idD3D12PipelineLibrary> cut = idD3D12PipelineLibrary::Open( gpu.device,
			std::vector<byte>( libraryFile.begin(), libraryFile.begin() + static_cast<ptrdiff_t>( libraryFile.size() / 2 ) ) );
		Check( cut && !cut->FromFile() && cut->Dirty(), "a file cut short: the same" );
	}
	CheckNoLayerErrors( gpu );
}

// r_pix 2: nested groups outlive a submit (ended in the list it closes, begun again in the next one);
// pushes past MAX_DEPTH and their pops are ignored, so the stack stays balanced.
void Test_WarpDebugGroups() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 );
	if ( !gpu ) {
		return;
	}
	idD3D12DebugGroups groups;
	groups.Push( gpu->List(), "frame" );
	groups.Push( gpu->List(), "MRB_DRAW_INTERACTIONS" );
	Check( groups.Depth() == 2 && groups.PushedThisFrame() == 2, "two nested groups" );
	groups.CloseAll( gpu->List() );
	gpu->SubmitChunk();
	groups.Reopen( gpu->List() );
	groups.Pop( &gpu->List() );
	Check( groups.Depth() == 1, "the inner group ends in the next list" );

	constexpr int PUSHES = idD3D12DebugGroups::MAX_DEPTH + 2;
	const std::string longName( idD3D12DebugGroups::MAX_NAME * 2, 'x' );
	for ( int i = 0; i < PUSHES; i++ ) {
		groups.Push( gpu->List(), longName.c_str() );
	}
	Check( groups.Depth() == idD3D12DebugGroups::MAX_DEPTH, "the stack stops at MAX_DEPTH" );
	for ( int i = 0; i < PUSHES; i++ ) {
		groups.Pop( &gpu->List() );
	}
	Check( groups.Depth() == 1, "the pops of the ignored pushes are ignored" );
	groups.Pop( &gpu->List() );
	groups.Pop( &gpu->List() );
	Check( groups.Depth() == 0, "a pop of an empty stack does nothing" );
	groups.ResetFrameCounters();
	Check( groups.PushedThisFrame() == 0, "the frame counter restarts" );
	gpu->WaitIdle();
	CheckNoLayerErrors( *gpu );
}

// r_pix 1 and the frame timer: a frame across two lists, a gap in the written slots. Ready reads Waited()
// only, so a finished GPU is not enough, and a slot not written in the set's last frame is never ready.
void Timestamps( idTestGpu & gpu ) {
	idD3D12GpuTimers timers( gpu.device, gpu.queue );
	if ( !Check( timers.Available() && timers.Frequency() > 0, "a direct queue has timestamps" ) ) {
		return;
	}
	constexpr int SET = 2;
	timers.BeginFrame( gpu.List() );
	timers.Write( gpu.List(), SET, 0 );
	gpu.SubmitChunk();
	timers.Write( gpu.List(), SET, 1 );
	timers.Write( gpu.List(), SET, 3 );
	timers.EndFrame( gpu.List() );
	Check( timers.WrittenThisFrame() == 3, "three slots written" );
	Check( !timers.Ready( SET, 0 ) && timers.FrameMicroseconds() == 0, "nothing is ready before its list is submitted" );
	const uint64 fence = gpu.SubmitChunk();
	gpu.FinishUnwaited();
	Check( !timers.Ready( SET, 1 ) && timers.FrameMicroseconds() == 0, "a finished GPU is not enough: Ready reads Waited() only" );
	std::array<uint64, 4> nanoseconds = {};
	timers.Read( SET, nanoseconds );
	Check( nanoseconds == std::array<uint64, 4>{}, "a set not ready reads zeros" );

	gpu.WaitFor( fence );
	Check( timers.Ready( SET, 0 ) && timers.Ready( SET, 1 ) && timers.Ready( SET, 3 ), "ready once the list is waited for" );
	Check( !timers.Ready( SET, 2 ) && !timers.Ready( SET - 1, 0 ) && !timers.Ready( SET, idD3D12GpuTimers::SLOTS_PER_SET ),
		"a slot not written is never ready" );
	timers.Read( SET, nanoseconds );
	Check( nanoseconds[0] > 0 && nanoseconds[0] <= nanoseconds[1] && nanoseconds[1] <= nanoseconds[3] && nanoseconds[2] == 0,
		"GPU order across the two lists; the unwritten slot reads 0" );
	const uint64 frameMicroseconds = timers.FrameMicroseconds();
	Check( frameMicroseconds * 1000 + 2000 >= nanoseconds[3] - nanoseconds[0], "the frame timer spans the stamps inside it, to its rounding" );

	timers.ResetFrameCounters();
	timers.Free();
	Check( !timers.Ready( SET, 0 ) && timers.WrittenThisFrame() == 0, "Free forgets the r_pix slots" );
	Check( timers.FrameMicroseconds() == frameMicroseconds, "the frame timer stays" );
	gpu.WaitIdle();
	CheckNoLayerErrors( gpu );
}

// ---- GL's rasterization rules through the real root signature, programs and PSOs -----------------------------

// A SIZE x SIZE RGBA8 + D24S8 target drawn as the recorder draws: base/shaders programs, GL clip space through
// idGLClipToD3D, so memory row r is GL row r (glReadPixels' order). Vertices, indexes and parms share one UPLOAD buffer.
class idTestRaster {
public:
	static constexpr int			SIZE = 64;
	struct draw_t {
		uint64						stateBits = 0;
		int							raster = 0;			// the cull, lines and strict depth bits of pipelineKey_t
		uint8						stencilRef = 0;
		uint8						depthBias = 0;		// a slot of Bias()
		std::pair<float, float>		depthBounds = { 0.0f, 1.0f };
		int							times = 1;
	};
	struct pixels_t {
		std::vector<uint32>			color;		// R | G << 8 | B << 16 | A << 24
		std::vector<uint32>			depth;		// the 24 bits
		std::vector<byte>			stencil;
		[[nodiscard]] static int	At( int column, int row ) { return row * SIZE + column; }
	};

	explicit						idTestRaster( idTestGpu & gpu );
									~idTestRaster();
									idTestRaster( const idTestRaster & ) = delete;
	idTestRaster &					operator=( const idTestRaster & ) = delete;
	[[nodiscard]] bool				Ready() const { return mapped != nullptr && readback && viewHeap && samplerHeap; }
	// Stages of base/shaders by GL's names ("depth.vertex"); nullptr when they do not build.
	[[nodiscard]] d3d12Program_t *	Program( const char * vertex, const char * fragment );
	void							Clear( const idVec4 & rgba, float depthValue, uint8 stencil );
	void							ClearDepth( float depthValue, const D3D12_RECT & rect );
	void							Draw( d3d12Program_t & program, const draw_t & draw, std::span<const idDrawVert> verts, std::span<const triIndex_t> indexes );
	[[nodiscard]] pixels_t			Read();		// waits for the GPU
	[[nodiscard]] idD3D12DepthBiasTable &	Bias() { return factory.Bias(); }

	std::array<idVec4, MAX_RENDER_PARMS>	parms = {};	// the MVP starts as the identity: positions are GL clip space

private:
	static constexpr uint64			UPLOAD_BYTES = 4 << 20;
	static constexpr uint64			ZERO_BYTES = 8192;		// the parms and joints of a stage without any
	[[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS	Put( const void * data, uint64 bytes );
	void							Target( D3D12_RESOURCE_STATES colorAfter, D3D12_RESOURCE_STATES depthAfter );

	idTestGpu &						gpu;
	idD3D12HlslCompiler				compiler;
	idD3D12RootSignature			root;
	idD3D12PipelineFactory			factory;
	idD3D12DescriptorPool			dsvPool;
	std::vector<std::unique_ptr<d3d12Program_t>>	programs;
	idComPtr<ID3D12Resource>		color, depth, upload, readback;
	d3d12ResourceState_t			colorState, depthState;
	d3d12Descriptor_t				rtv, dsv;
	idComPtr<ID3D12DescriptorHeap>	viewHeap, samplerHeap;
	std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 3>	footprints = {};	// colour, depth plane, stencil plane
	byte *							mapped = nullptr;
	uint64							used = ZERO_BYTES;
};

idTestRaster::idTestRaster( idTestGpu & owner ) :
	gpu( owner ),
	compiler( owner.library, RepoReader(), false ),
	root( owner.device, owner.library ),
	factory( owner.device, root, nullptr, nullptr ),
	dsvPool( owner.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV ) {
	ID3D12Device2 & device = gpu.device.Get();
	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC colorDesc = {};
	colorDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	colorDesc.Width = SIZE;
	colorDesc.Height = SIZE;
	colorDesc.DepthOrArraySize = 1;
	colorDesc.MipLevels = 1;
	colorDesc.Format = d3d12TargetId_t::Of( false, 1 ).ColorView();
	colorDesc.SampleDesc.Count = 1;
	colorDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	D3D12_RESOURCE_DESC depthDesc = colorDesc;
	depthDesc.Format = d3d12TargetId_t::DEPTH;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE colorClear = {};
	colorClear.Format = colorDesc.Format;
	D3D12_CLEAR_VALUE depthClear = {};
	depthClear.Format = depthDesc.Format;
	depthClear.DepthStencil = { 1.0f, 0 };
	if ( !root.Get()
			|| !gpu.device.Check( device.CreateCommittedResource( &defaultHeap, D3D12_HEAP_FLAG_NONE, &colorDesc, D3D12_RESOURCE_STATE_RENDER_TARGET,
				&colorClear, IID_PPV_ARGS( &color ) ), "CreateCommittedResource (test colour)" )
			|| !gpu.device.Check( device.CreateCommittedResource( &defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
				&depthClear, IID_PPV_ARGS( &depth ) ), "CreateCommittedResource (test depth)" ) ) {
		Check( false, "the test target" );
		return;
	}
	colorState.current = D3D12_RESOURCE_STATE_RENDER_TARGET;
	depthState.current = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	rtv = gpu.rtvPool.Allocate();
	dsv = dsvPool.Allocate();
	device.CreateRenderTargetView( color.Get(), nullptr, gpu.rtvPool.Cpu( rtv ) );
	device.CreateDepthStencilView( depth.Get(), nullptr, dsvPool.Cpu( dsv ) );

	UINT rows[2] = {};
	UINT64 rowBytes[2] = {};
	UINT64 colorBytes = 0;
	device.GetCopyableFootprints( &colorDesc, 0, 1, 0, &footprints[0], rows, rowBytes, &colorBytes );
	const UINT64 depthBase = ( colorBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1 ) & ~UINT64( D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1 );
	device.GetCopyableFootprints( &depthDesc, 0, 2, depthBase, &footprints[1], rows, rowBytes, nullptr );
	const uint64 readbackBytes = footprints[2].Offset + static_cast<uint64>( footprints[2].Footprint.RowPitch ) * SIZE;
	readback = D3D12_CreateBuffer( gpu.device, D3D12_HEAP_TYPE_READBACK, readbackBytes, D3D12_RESOURCE_STATE_COPY_DEST, L"test raster readback" );
	upload = D3D12_CreateBuffer( gpu.device, D3D12_HEAP_TYPE_UPLOAD, UPLOAD_BYTES, D3D12_RESOURCE_STATE_GENERIC_READ, L"test raster upload" );

	// what every index of b3 reads here, 0: a null view and a point sampler
	const auto heap = [&]( D3D12_DESCRIPTOR_HEAP_TYPE type ) {
		const D3D12_DESCRIPTOR_HEAP_DESC heapDesc = { type, MAX_TEXTURE_SLOTS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
		idComPtr<ID3D12DescriptorHeap> created;
		gpu.device.Check( device.CreateDescriptorHeap( &heapDesc, IID_PPV_ARGS( &created ) ), "CreateDescriptorHeap (test raster)" );
		return created;
	};
	viewHeap = heap( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
	samplerHeap = heap( D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER );
	if ( !viewHeap || !samplerHeap || !upload ) {
		return;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC none = {};
	none.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	none.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	none.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	none.Texture2D.MipLevels = 1;
	const D3D12_SAMPLER_DESC sampler = D3D12_SamplerFor( D3D12_PackSampler( POINT_CLAMP ), false );
	D3D12_CPU_DESCRIPTOR_HANDLE view = viewHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE samplerSlot = samplerHeap->GetCPUDescriptorHandleForHeapStart();
	for ( int slot = 0; slot < MAX_TEXTURE_SLOTS; slot++ ) {
		device.CreateShaderResourceView( nullptr, &none, view );
		device.CreateSampler( &sampler, samplerSlot );
		view.ptr += device.GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
		samplerSlot.ptr += device.GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER );
	}
	const D3D12_RANGE nothingRead = { 0, 0 };
	void * cpu = nullptr;
	if ( gpu.device.Check( upload->Map( 0, &nothingRead, &cpu ), "Map (test raster upload)" ) ) {
		mapped = static_cast<byte *>( cpu );
		memset( mapped, 0, ZERO_BYTES );
	}
	for ( int row = 0; row < 4; row++ ) {
		idVec4 & mvp = parms[RENDERPARM_MVPMATRIX_X + row];
		mvp.Zero();
		mvp[row] = 1.0f;
	}
}

idTestRaster::~idTestRaster() {
	if ( !gpu.device.IsLost() ) {
		gpu.WaitIdle();
	}
	if ( rtv.IsValid() ) {
		gpu.rtvPool.Free( rtv );
	}
}

d3d12Program_t * idTestRaster::Program( const char * vertexName, const char * fragmentName ) {
	const auto vertex = RepoStage( compiler, vertexName, shaderStageType_t::VERTEX );
	const auto fragment = RepoStage( compiler, fragmentName, shaderStageType_t::FRAGMENT );
	if ( !Check( vertex && fragment, va( "%s and %s build", vertexName, fragmentName ) ) ) {
		return nullptr;
	}
	auto program = D3D12_LinkProgram( *vertex, &*fragment );
	if ( !Check( program.has_value(), va( "%s and %s link", vertexName, fragmentName ) ) ) {
		return nullptr;
	}
	programs.push_back( std::make_unique<d3d12Program_t>( std::move( *program ) ) );
	programs.back()->dense = static_cast<uint16>( programs.size() );
	return programs.back().get();
}

D3D12_GPU_VIRTUAL_ADDRESS idTestRaster::Put( const void * data, uint64 bytes ) {
	const uint64 offset = ( used + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1 ) & ~uint64( D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1 );
	if ( !Check( offset + bytes <= UPLOAD_BYTES, "the test upload buffer is full" ) ) {
		return upload->GetGPUVirtualAddress();
	}
	memcpy( mapped + offset, data, static_cast<size_t>( bytes ) );
	used = offset + bytes;
	return upload->GetGPUVirtualAddress() + offset;
}

void idTestRaster::Target( D3D12_RESOURCE_STATES colorAfter, D3D12_RESOURCE_STATES depthAfter ) {
	gpu.barriers.Transition( *color.Get(), colorState, colorAfter );
	gpu.barriers.Transition( *depth.Get(), depthState, depthAfter );
	gpu.barriers.Flush();
}

void idTestRaster::Clear( const idVec4 & rgba, float depthValue, uint8 stencil ) {
	Target( D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_DEPTH_WRITE );
	gpu.List().ClearRenderTargetView( gpu.rtvPool.Cpu( rtv ), rgba.ToFloatPtr(), 0, nullptr );
	gpu.List().ClearDepthStencilView( dsvPool.Cpu( dsv ), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depthValue, stencil, 0, nullptr );
}

void idTestRaster::ClearDepth( float depthValue, const D3D12_RECT & rect ) {
	Target( D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_DEPTH_WRITE );
	gpu.List().ClearDepthStencilView( dsvPool.Cpu( dsv ), D3D12_CLEAR_FLAG_DEPTH, depthValue, 0, 1, &rect );
}

void idTestRaster::Draw( d3d12Program_t & program, const draw_t & draw, std::span<const idDrawVert> verts, std::span<const triIndex_t> indexes ) {
	pipelineKey_t key = {};
	key.stateBits = draw.stateBits & GLS_PIPELINE_BITS;
	key.layout = LAYOUT_DRAW_VERT;
	key.raster = static_cast<uint8>( draw.raster | ( d3d12TargetId_t::Of( false, 1 ).value << D3D12_KEY_TARGET_SHIFT ) );
	key.depthBias = draw.depthBias;
	key.program = program.dense;
	ID3D12PipelineState * pso = factory.Get( program, D3D12_NormalizeKey( key ) );
	if ( !Check( pso != nullptr, va( "a PSO for 0x%016llx", key.stateBits ) ) ) {
		return;
	}
	std::array<idVec4, MAX_RENDER_PARMS> packed;
	const auto stageParms = [&]( const shaderStageInfo_t & info ) {
		if ( info.numParms == 0 ) {
			return upload->GetGPUVirtualAddress();
		}
		R_PackRenderParms( packed.data(), parms.data(), info );
		return Put( packed.data(), static_cast<uint64>( info.numParms ) * sizeof( idVec4 ) );
	};
	const D3D12_GPU_VIRTUAL_ADDRESS vertexParms = stageParms( program.vertexInfo );
	const D3D12_GPU_VIRTUAL_ADDRESS fragmentParms = stageParms( program.fragmentInfo );
	const D3D12_VERTEX_BUFFER_VIEW vertexView = { Put( verts.data(), verts.size_bytes() ), static_cast<UINT>( verts.size_bytes() ), sizeof( idDrawVert ) };
	const D3D12_INDEX_BUFFER_VIEW indexView = { Put( indexes.data(), indexes.size_bytes() ), static_cast<UINT>( indexes.size_bytes() ), DXGI_FORMAT_R16_UINT };

	Target( D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_DEPTH_WRITE );
	ID3D12GraphicsCommandList1 & list = gpu.List();
	ID3D12DescriptorHeap * heaps[] = { viewHeap.Get(), samplerHeap.Get() };
	list.SetDescriptorHeaps( 2, heaps );
	list.SetGraphicsRootSignature( root.Get() );
	list.SetPipelineState( pso );
	const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = gpu.rtvPool.Cpu( rtv );
	const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvPool.Cpu( dsv );
	list.OMSetRenderTargets( 1, &rtvHandle, FALSE, &dsvHandle );
	const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>( SIZE ), static_cast<float>( SIZE ), 0.0f, 1.0f };
	const D3D12_RECT scissor = { 0, 0, SIZE, SIZE };
	list.RSSetViewports( 1, &viewport );
	list.RSSetScissorRects( 1, &scissor );
	list.IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	list.OMSetStencilRef( draw.stencilRef );
	if ( gpu.device.Features().depthBounds ) {
		list.OMSetDepthBounds( draw.depthBounds.first, draw.depthBounds.second );
	}
	list.SetGraphicsRootConstantBufferView( D3D12_Root( d3d12RootSlot_t::VERTEX_PARMS ), vertexParms );
	list.SetGraphicsRootConstantBufferView( D3D12_Root( d3d12RootSlot_t::FRAGMENT_PARMS ), fragmentParms );
	list.SetGraphicsRootConstantBufferView( D3D12_Root( d3d12RootSlot_t::JOINTS ), upload->GetGPUVirtualAddress() );
	const d3d12DrawResources_t resources;
	list.SetGraphicsRoot32BitConstants( D3D12_Root( d3d12RootSlot_t::DRAW_RESOURCES ), d3d12DrawResources_t::COUNT, resources.indices.data(), 0 );
	list.IASetVertexBuffers( 0, 1, &vertexView );
	list.IASetIndexBuffer( &indexView );
	for ( int i = 0; i < draw.times; i++ ) {
		list.DrawIndexedInstanced( static_cast<UINT>( indexes.size() ), 1, 0, 0, 0 );
	}
}

idTestRaster::pixels_t idTestRaster::Read() {
	pixels_t pixels;
	Target( D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE );
	for ( size_t i = 0; i < footprints.size(); i++ ) {
		D3D12_TEXTURE_COPY_LOCATION destination = {};
		destination.pResource = readback.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		destination.PlacedFootprint = footprints[i];
		D3D12_TEXTURE_COPY_LOCATION source = {};
		source.pResource = ( i == 0 ) ? color.Get() : depth.Get();
		source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		source.SubresourceIndex = ( i == 2 ) ? 1 : 0;
		gpu.List().CopyTextureRegion( &destination, 0, 0, 0, &source, nullptr );
	}
	gpu.WaitIdle();
	used = ZERO_BYTES;
	const D3D12_RANGE everything = { 0, static_cast<SIZE_T>( footprints[2].Offset + static_cast<uint64>( footprints[2].Footprint.RowPitch ) * SIZE ) };
	void * cpu = nullptr;
	if ( !gpu.device.Check( readback->Map( 0, &everything, &cpu ), "Map (test raster readback)" ) ) {
		return pixels;
	}
	const byte * bytes = static_cast<const byte *>( cpu );
	const auto texel = [&]( size_t plane, int column, int row, int texelBytes ) {
		return bytes + footprints[plane].Offset + static_cast<size_t>( row ) * footprints[plane].Footprint.RowPitch + static_cast<size_t>( column * texelBytes );
	};
	pixels.color.resize( SIZE * SIZE );
	pixels.depth.resize( SIZE * SIZE );
	pixels.stencil.resize( SIZE * SIZE );
	for ( int row = 0; row < SIZE; row++ ) {
		for ( int column = 0; column < SIZE; column++ ) {
			const int at = pixels_t::At( column, row );
			memcpy( &pixels.color[at], texel( 0, column, row, 4 ), 4 );
			memcpy( &pixels.depth[at], texel( 1, column, row, 4 ), 4 );
			pixels.depth[at] &= 0xFFFFFFu;
			pixels.stencil[at] = *texel( 2, column, row, 1 );
		}
	}
	const D3D12_RANGE nothingWritten = { 0, 0 };
	readback->Unmap( 0, &nothingWritten );
	return pixels;
}

[[nodiscard]] idDrawVert ClipVertex( float x, float y, float z ) {
	idDrawVert vertex;
	vertex.Clear();
	vertex.xyz.Set( x, y, z );
	return vertex;
}

// Counter-clockwise in GL's window (y up): a GL front face. QUAD_CW winds it the other way.
[[nodiscard]] std::array<idDrawVert, 4> Quad( float x0, float y0, float x1, float y1, float z0, float z1 ) {
	return { ClipVertex( x0, y0, z0 ), ClipVertex( x1, y0, z1 ), ClipVertex( x1, y1, z1 ), ClipVertex( x0, y1, z0 ) };
}
[[nodiscard]] std::array<idDrawVert, 4> Quad( float x0, float y0, float x1, float y1, float z = 0.0f ) {
	return Quad( x0, y0, x1, y1, z, z );
}
constexpr std::array<triIndex_t, 6> QUAD_CCW = { 0, 1, 2, 0, 2, 3 };
constexpr std::array<triIndex_t, 6> QUAD_CW = { 0, 2, 1, 0, 3, 2 };
constexpr uint32 RGBA_RED = 0xFF0000FFu;
constexpr uint32 RGBA_GREEN = 0xFF00FF00u;

// The GL clip coordinates of the centre of a target pixel, its memory row taken as GL's row.
[[nodiscard]] idVec2 GlCentre( int column, int row ) {
	constexpr float HALF = idTestRaster::SIZE * 0.5f;
	return idVec2( ( column + 0.5f ) / HALF - 1.0f, ( row + 0.5f ) / HALF - 1.0f );
}

// The distance of p from the edge a → b, positive on its left (inside a counter-clockwise triangle).
[[nodiscard]] float EdgeDistance( const idVec2 & a, const idVec2 & b, const idVec2 & p ) {
	const idVec2 edge = b - a;
	return ( edge.x * ( p.y - a.y ) - edge.y * ( p.x - a.x ) ) / edge.Length();
}

// A quad and a triangle in GL clip space land where glReadPixels finds them in GL's frame.
void RasterGlRows( idTestGpu & gpu ) {
	idTestRaster raster( gpu );
	d3d12Program_t * program = raster.Ready() ? raster.Program( "color.vertex", "color.pixel" ) : nullptr;
	if ( program == nullptr ) {
		return;
	}
	raster.Clear( idVec4( 0.0f, 0.0f, 0.0f, 0.0f ), 1.0f, 0 );
	raster.parms[RENDERPARM_COLOR] = idVec4( 1.0f, 0.0f, 0.0f, 1.0f );
	raster.Draw( *program, { .stateBits = GLS_DEPTHFUNC_ALWAYS }, Quad( -1.0f, 0.0f, 0.0f, 1.0f ), QUAD_CCW );
	const idVec2 corners[3] = { idVec2( 0.1f, -0.9f ), idVec2( 0.9f, -0.9f ), idVec2( 0.9f, -0.15f ) };
	const std::array<idDrawVert, 3> triangle = { ClipVertex( corners[0].x, corners[0].y, 0.0f ), ClipVertex( corners[1].x, corners[1].y, 0.0f ),
		ClipVertex( corners[2].x, corners[2].y, 0.0f ) };
	constexpr std::array<triIndex_t, 3> TRIANGLE = { 0, 1, 2 };
	raster.parms[RENDERPARM_COLOR] = idVec4( 0.0f, 1.0f, 0.0f, 1.0f );
	raster.Draw( *program, { .stateBits = GLS_DEPTHFUNC_ALWAYS }, triangle, TRIANGLE );
	const idTestRaster::pixels_t pixels = raster.Read();
	if ( !Check( !pixels.color.empty(), "the read back" ) ) {
		return;
	}
	int wrong = 0;
	for ( int row = 0; row < idTestRaster::SIZE; row++ ) {
		for ( int column = 0; column < idTestRaster::SIZE; column++ ) {
			const idVec2 p = GlCentre( column, row );
			const float inside = Min( Min( EdgeDistance( corners[0], corners[1], p ), EdgeDistance( corners[1], corners[2], p ) ),
				EdgeDistance( corners[2], corners[0], p ) );
			if ( idMath::Fabs( inside ) < 0.01f ) {
				continue;	// on an edge: the fill rule decides
			}
			const uint32 expected = ( inside > 0.0f ) ? RGBA_GREEN : ( p.x < 0.0f && p.y > 0.0f ) ? RGBA_RED : 0u;
			const uint32 drawn = pixels.color[idTestRaster::pixels_t::At( column, row )];
			wrong += ( drawn != expected ) ? 1 : 0;
			Check( drawn == expected, va( "memory row %d column %d: 0x%08x, GL 0x%08x", row, column, drawn, expected ) );
		}
	}
	printf( "  %d pixels off GL's rows\n", wrong );
	CheckNoLayerErrors( gpu );
}

// R_PipelineCull and FrontCounterClockwise FALSE against API_Cull: GL's front faces wind counter-clockwise in its window,
// a mirror swaps the culled face, and the two-sided stencil ops of a shadow volume go to GL's faces.
void RasterCull( idTestGpu & gpu ) {
	idTestRaster raster( gpu );
	d3d12Program_t * program = raster.Ready() ? raster.Program( "color.vertex", "color.pixel" ) : nullptr;
	if ( program == nullptr ) {
		return;
	}
	const std::array<idDrawVert, 4> left = Quad( -0.9f, -0.5f, -0.1f, 0.5f );		// drawn counter-clockwise
	const std::array<idDrawVert, 4> right = Quad( 0.1f, -0.5f, 0.9f, 0.5f );		// drawn clockwise
	const int leftPixel = idTestRaster::pixels_t::At( idTestRaster::SIZE / 4, idTestRaster::SIZE / 2 );
	const int rightPixel = idTestRaster::pixels_t::At( idTestRaster::SIZE * 3 / 4, idTestRaster::SIZE / 2 );
	raster.parms[RENDERPARM_COLOR] = idVec4( 1.0f, 1.0f, 1.0f, 1.0f );
	for ( const cullType_t cullType : { CT_FRONT_SIDED, CT_BACK_SIDED, CT_TWO_SIDED } ) {
		for ( const bool mirror : { false, true } ) {
			for ( const bool enabled : { true, false } ) {
				// API_Cull: glCullFace( GL_FRONT ) for FRONT_SIDED, GL_BACK for BACK_SIDED, swapped in a mirror; GL_CULL_FACE off for
				// TWO_SIDED, and while a two-sided pass left it off
				const bool culling = enabled && cullType != CT_TWO_SIDED;
				const bool cullsGlFront = culling && ( ( cullType == CT_FRONT_SIDED ) != mirror );
				const bool cullsGlBack = culling && !cullsGlFront;
				raster.Clear( idVec4( 0.0f, 0.0f, 0.0f, 0.0f ), 1.0f, 0 );
				const idTestRaster::draw_t draw = { .stateBits = GLS_DEPTHFUNC_ALWAYS, .raster = R_PipelineCull( cullType, enabled, mirror ) };
				raster.Draw( *program, draw, left, QUAD_CCW );
				raster.Draw( *program, draw, right, QUAD_CW );
				const idTestRaster::pixels_t pixels = raster.Read();
				if ( pixels.color.empty() ) {
					return;
				}
				const bool leftDrawn = pixels.color[leftPixel] != 0;
				const bool rightDrawn = pixels.color[rightPixel] != 0;
				Check( leftDrawn == !cullsGlFront && rightDrawn == !cullsGlBack, va( "cull type %d, mirror %d, culling %d: counter-clockwise %s, "
					"clockwise %s", cullType, mirror ? 1 : 0, enabled ? 1 : 0, leftDrawn ? "drawn" : "culled", rightDrawn ? "drawn" : "culled" ) );
			}
		}
	}

	raster.Clear( idVec4( 0.0f, 0.0f, 0.0f, 0.0f ), 1.0f, 128 );
	const idTestRaster::draw_t volume = { .stateBits = GLS_STENCIL_SHADOW_ZPASS | GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK };
	raster.Draw( *program, volume, left, QUAD_CCW );
	raster.Draw( *program, volume, right, QUAD_CW );
	const idTestRaster::pixels_t pixels = raster.Read();
	if ( !pixels.stencil.empty() ) {
		Check( pixels.stencil[leftPixel] == 129 && pixels.stencil[rightPixel] == 127, va( "shadow volume: front face %d (GL 129), back face %d (GL 127)",
			pixels.stencil[leftPixel], pixels.stencil[rightPixel] ) );
	}
	CheckNoLayerErrors( gpu );
}

// E17: the interaction pass with depth EQUAL hits every pixel the depth pass wrote, though interaction.vertex is another
// program than depth.vertex: precise keeps the arithmetic of the position the same in both.
void RasterDepthEqual( idTestGpu & gpu ) {
	idTestRaster raster( gpu );
	d3d12Program_t * depthPass = raster.Ready() ? raster.Program( "depth.vertex", "depth.pixel" ) : nullptr;
	d3d12Program_t * interaction = raster.Ready() ? raster.Program( "interaction.vertex", "interaction.pixel" ) : nullptr;
	if ( depthPass == nullptr || interaction == nullptr ) {
		return;
	}
	uint32 seed = 7;
	const auto random = [&seed]( float low, float high ) {
		seed = seed * 1664525u + 1013904223u;
		return low + ( high - low ) * static_cast<float>( seed >> 8 ) / static_cast<float>( 1 << 24 );
	};
	for ( idVec4 & parm : raster.parms ) {
		parm.Set( random( -1.0f, 1.0f ), random( -1.0f, 1.0f ), random( -1.0f, 1.0f ), random( -1.0f, 1.0f ) );
	}
	raster.parms[RENDERPARM_MVPMATRIX_X].Set( 0.93f, 0.071f, -0.013f, 0.0123f );
	raster.parms[RENDERPARM_MVPMATRIX_Y].Set( -0.061f, 0.97f, 0.027f, -0.0071f );
	raster.parms[RENDERPARM_MVPMATRIX_Z].Set( 0.031f, -0.043f, 0.89f, 0.0137f );
	raster.parms[RENDERPARM_MVPMATRIX_W].Set( 0.011f, -0.023f, 0.037f, 1.0f );

	constexpr int CELLS = 16;
	std::vector<idDrawVert> grid;
	for ( int y = 0; y <= CELLS; y++ ) {
		for ( int x = 0; x <= CELLS; x++ ) {
			idDrawVert vertex = ClipVertex( -1.2f + 2.4f * x / CELLS, -1.2f + 2.4f * y / CELLS, random( -0.6f, 0.6f ) );
			vertex.SetTexCoord( random( 0.0f, 4.0f ), random( 0.0f, 4.0f ) );
			idVec3 normal( random( -1.0f, 1.0f ), random( -1.0f, 1.0f ), 1.0f );
			idVec3 tangent( 1.0f, random( -1.0f, 1.0f ), random( -1.0f, 1.0f ) );
			normal.Normalize();
			tangent.Normalize();
			vertex.SetNormal( normal );
			vertex.SetTangent( tangent );
			vertex.SetColor( 0xFF8040C0u );
			grid.push_back( vertex );
		}
	}
	std::vector<triIndex_t> indexes;
	for ( int y = 0; y < CELLS; y++ ) {
		for ( int x = 0; x < CELLS; x++ ) {
			const int corner = y * ( CELLS + 1 ) + x;
			for ( const int offset : { 0, 1, CELLS + 2, 0, CELLS + 2, CELLS + 1 } ) {
				indexes.push_back( static_cast<triIndex_t>( corner + offset ) );
			}
		}
	}
	raster.Clear( idVec4( 0.0f, 0.0f, 0.0f, 0.0f ), 1.0f, 0 );
	raster.Draw( *depthPass, { .stateBits = GLS_DEPTHFUNC_LESS | GLS_COLORMASK | GLS_ALPHAMASK }, grid, indexes );
	raster.Draw( *interaction, { .stateBits = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL | GLS_STENCIL_OP_PASS_INCR },
		grid, indexes );
	const idTestRaster::pixels_t pixels = raster.Read();
	int covered = 0;
	int missed = 0;
	for ( size_t i = 0; i < pixels.depth.size(); i++ ) {
		if ( pixels.depth[i] != 0xFFFFFFu ) {
			covered++;
			missed += ( pixels.stencil[i] != 1 ) ? 1 : 0;
		}
	}
	printf( "  %d of %d covered pixels fail EQUAL\n", missed, covered );
	Check( covered == idTestRaster::SIZE * idTestRaster::SIZE, va( "the grid covers %d pixels", covered ) );
	Check( missed == 0, va( "%d pixels fail EQUAL", missed ) );
	CheckNoLayerErrors( gpu );
}

// The depth test rejects the farther of two quads in either order, with BFG's LESS (GL_LEQUAL) and the strict GL_LESS
// of GL's initial state; EQUAL lets only the stored depth through.
void RasterDepthOrder( idTestGpu & gpu ) {
	idTestRaster raster( gpu );
	d3d12Program_t * program = raster.Ready() ? raster.Program( "color.vertex", "color.pixel" ) : nullptr;
	if ( program == nullptr ) {
		return;
	}
	const int centre = idTestRaster::pixels_t::At( idTestRaster::SIZE / 2, idTestRaster::SIZE / 2 );
	const idVec4 red( 1.0f, 0.0f, 0.0f, 1.0f );
	const idVec4 green( 0.0f, 1.0f, 0.0f, 1.0f );
	for ( const int strict : { 0, static_cast<int>( D3D12_KEY_STRICT_DEPTH ) } ) {
		for ( const bool nearFirst : { true, false } ) {
			raster.Clear( idVec4( 0.0f, 0.0f, 0.0f, 0.0f ), 1.0f, 0 );
			const idTestRaster::draw_t draw = { .stateBits = GLS_DEPTHFUNC_LESS, .raster = strict };
			raster.parms[RENDERPARM_COLOR] = nearFirst ? red : green;
			raster.Draw( *program, draw, Quad( -1.0f, -1.0f, 1.0f, 1.0f, nearFirst ? -0.5f : 0.5f ), QUAD_CCW );
			raster.parms[RENDERPARM_COLOR] = nearFirst ? green : red;
			raster.Draw( *program, draw, Quad( -1.0f, -1.0f, 1.0f, 1.0f, nearFirst ? 0.5f : -0.5f ), QUAD_CCW );
			raster.parms[RENDERPARM_COLOR] = idVec4( 0.0f, 0.0f, 1.0f, 1.0f );
			raster.Draw( *program, { .stateBits = GLS_DEPTHFUNC_EQUAL | GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK | GLS_STENCIL_OP_PASS_INCR },
				Quad( -1.0f, -1.0f, 1.0f, 1.0f, 0.5f ), QUAD_CCW );
			const idTestRaster::pixels_t pixels = raster.Read();
			if ( !Check( !pixels.color.empty(), "the read back" ) ) {
				return;
			}
			Check( pixels.color[centre] == RGBA_RED && pixels.stencil[centre] == 0, va( "strict %d, near quad %s: 0x%08x, stencil %d (GL red, 0)",
				strict != 0 ? 1 : 0, nearFirst ? "first" : "second", pixels.color[centre], pixels.stencil[centre] ) );
		}
	}
	CheckNoLayerErrors( gpu );
}

// GL's INCR and DECR saturate (D3D12's INCR_SAT, DECR_SAT); INCR_WRAP and DECR_WRAP wrap (D3D12's INCR, DECR).
void RasterStencilOps( idTestGpu & gpu ) {
	idTestRaster raster( gpu );
	d3d12Program_t * program = raster.Ready() ? raster.Program( "color.vertex", "color.pixel" ) : nullptr;
	if ( program == nullptr ) {
		return;
	}
	struct row_t {
		const char *				name;
		uint64						op;
		uint8						start;
		int							times;
		byte						gl;
	};
	const row_t rows[] = {
		{ "INCR", GLS_STENCIL_OP_PASS_INCR, 0, 300, 255 },
		{ "INCR_WRAP", GLS_STENCIL_OP_PASS_INCR_WRAP, 0, 300, 300 - 256 },
		{ "DECR", GLS_STENCIL_OP_PASS_DECR, 2, 5, 0 },
		{ "DECR_WRAP", GLS_STENCIL_OP_PASS_DECR_WRAP, 2, 5, 253 },
	};
	for ( const row_t & row : rows ) {
		raster.Clear( idVec4( 0.0f, 0.0f, 0.0f, 0.0f ), 1.0f, row.start );
		raster.Draw( *program, { .stateBits = row.op | GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK, .times = row.times },
			Quad( -1.0f, -1.0f, 1.0f, 1.0f ), QUAD_CCW );
		const idTestRaster::pixels_t pixels = raster.Read();
		const auto wrong = std::ranges::count_if( pixels.stencil, [&row]( byte s ) { return s != row.gl; } );
		Check( !pixels.stencil.empty() && wrong == 0, va( "%s %d times from %d: %d, GL %d (%d pixels off)", row.name, row.times, row.start,
			pixels.stencil.empty() ? -1 : pixels.stencil[0], row.gl, static_cast<int>( wrong ) ) );
	}
	CheckNoLayerErrors( gpu );
}

// The depth bounds test reads the stored depth, as EXT_depth_bounds_test does, inclusive; (0, 1) lets everything through.
void RasterDepthBounds( idTestGpu & gpu ) {
	if ( !gpu.device.Features().depthBounds ) {
		printf( "  skipped: no depth bounds test (OPTIONS2; WARP's turns the depth test off)\n" );
		return;
	}
	idTestRaster raster( gpu );
	d3d12Program_t * program = raster.Ready() ? raster.Program( "color.vertex", "color.pixel" ) : nullptr;
	if ( program == nullptr ) {
		return;
	}
	struct row_t {
		float						min, max;
		bool						left, right;	// stored 0.25 and 0.75
	};
	const row_t rows[] = {
		{ 0.5f, 1.0f, false, true },
		{ 0.0f, 0.5f, true, false },
		{ 0.0f, 1.0f, true, true },
		{ 0.3f, 0.7f, false, false },
		{ 0.2f, 0.8f, true, true },
	};
	constexpr int HALF = idTestRaster::SIZE / 2;
	raster.parms[RENDERPARM_COLOR] = idVec4( 1.0f, 0.0f, 0.0f, 1.0f );
	for ( const row_t & row : rows ) {
		raster.Clear( idVec4( 0.0f, 0.0f, 0.0f, 0.0f ), 1.0f, 0 );
		raster.ClearDepth( 0.25f, { 0, 0, HALF, idTestRaster::SIZE } );
		raster.ClearDepth( 0.75f, { HALF, 0, idTestRaster::SIZE, idTestRaster::SIZE } );
		// the fragments' own depth, 0.05, is outside every bound but the first rows': a test of it would fail them
		raster.Draw( *program, { .stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK, .depthBounds = { row.min, row.max } },
			Quad( -1.0f, -1.0f, 1.0f, 1.0f, -0.9f ), QUAD_CCW );
		const idTestRaster::pixels_t pixels = raster.Read();
		int wrong = 0;
		for ( int y = 0; y < idTestRaster::SIZE && !pixels.color.empty(); y++ ) {
			for ( int x = 0; x < idTestRaster::SIZE; x++ ) {
				const bool expected = ( x < HALF ) ? row.left : row.right;
				wrong += ( ( pixels.color[idTestRaster::pixels_t::At( x, y )] == RGBA_RED ) != expected ) ? 1 : 0;
			}
		}
		Check( !pixels.color.empty() && wrong == 0, va( "bounds %.2f .. %.2f: %d pixels off", row.min, row.max, wrong ) );
	}
	CheckNoLayerErrors( gpu );
}

// glPolygonOffset( factor, units ) with r = 2^-24 on D24: the slot's DepthBias moves the stored depth by units steps, and its
// slope bias by factor times the depth slope per pixel.
void RasterDepthBias( idTestGpu & gpu ) {
	idTestRaster raster( gpu );
	d3d12Program_t * program = raster.Ready() ? raster.Program( "color.vertex", "color.pixel" ) : nullptr;
	if ( program == nullptr ) {
		return;
	}
	const int centre = idTestRaster::pixels_t::At( idTestRaster::SIZE / 2, idTestRaster::SIZE / 2 );
	const auto depthAt = [&]( float factor, float units, float z0, float z1 ) -> int64 {
		const uint8 slot = ( factor != 0.0f || units != 0.0f ) ? raster.Bias().Slot( factor, units ) : 0;
		raster.Clear( idVec4( 0.0f, 0.0f, 0.0f, 0.0f ), 1.0f, 0 );
		raster.Draw( *program, { .stateBits = GLS_DEPTHFUNC_ALWAYS | ( slot != 0 ? GLS_POLYGON_OFFSET : 0 ), .depthBias = slot },
			Quad( -1.0f, -1.0f, 1.0f, 1.0f, z0, z1 ), QUAD_CCW );
		const idTestRaster::pixels_t pixels = raster.Read();
		return pixels.depth.empty() ? -1 : static_cast<int64>( pixels.depth[centre] );
	};
	const int64 flat = depthAt( 0.0f, 0.0f, 0.0f, 0.0f );
	const int64 up = depthAt( 0.0f, 64.0f, 0.0f, 0.0f ) - flat;
	const int64 down = depthAt( 0.0f, -64.0f, 0.0f, 0.0f ) - flat;
	printf( "  units 64: %+lld steps, units -64: %+lld steps\n", up, down );
	Check( flat > 0 && std::abs( up - 64 ) <= 1 && std::abs( down + 64 ) <= 1, va( "units 64 move the depth %lld and %lld steps", up, down ) );

	// z from -0.5 to 0.5 across the target: a depth slope of 0.5 / SIZE per pixel
	const int64 sloped = depthAt( 0.0f, 0.0f, -0.5f, 0.5f );
	const int64 slopeBias = depthAt( 2.0f, 0.0f, -0.5f, 0.5f ) - sloped;
	const double gl = 2.0 * 0.5 / idTestRaster::SIZE * 16777215.0;
	printf( "  factor 2: %+lld steps, GL %+.0f\n", slopeBias, gl );
	Check( std::abs( static_cast<double>( slopeBias ) - gl ) <= gl * 0.02, va( "factor 2 moves the depth %lld steps, GL %.0f", slopeBias, gl ) );
	CheckNoLayerErrors( gpu );
}

// ---- screen copies -----------------------------------------------------------------------------------------

// P1 and T6 without a draw: the two surfaces of a copy target, the sampler GL's colour copy sets, and the
// incomplete texture a mipmap filter makes of a copy at another size.
void Test_WarpCopyTargets() {
	const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 );
	if ( !gpu ) {
		return;
	}
	idD3D12TextureStore & store = gpu->textures;
	store.CreateDefaults();
	const uint64 bytesBefore = store.Bytes();
	const samplerDesc_t trilinear = { samplerFilter_t::LINEAR_MIP_LINEAR, samplerAddress_t::REPEAT, 4.0f, -0.5f };
	const textureHandle_t color = store.Create( Opts( FMT_RGBA8, 16, 16, 5 ), textureRole_t::COLOR_COPY, trilinear, "_currentRender" );
	const textureHandle_t depth = store.Create( Opts( FMT_DEPTH, 16, 16, 1 ), textureRole_t::DEPTH_COPY, POINT_CLAMP, "_currentDepth" );
	if ( !Check( color != textureHandle_t::NONE && depth != textureHandle_t::NONE, "Create" ) ) {
		return;
	}
	const d3d12Texture_t & texture = store.Get( color );
	const ID3D12Resource * created = texture.surface.resource.Get();

	store.RecordUploads( true );
	const std::vector<byte> pixels = Pattern( 16 * 16 * 4, 7 );
	store.Upload( color, 0, 0, 16, 16, pixels.data(), 16 );
	Check( !texture.uploads.empty(), "the upload is recorded" );
	Check( store.PrepareCopyTarget( color, d3d12CopyKind_t::COLOR, 16, 16 ) == &store.Get( color ).surface, "a copy target" );
	Check( texture.surface.resource.Get() == created && texture.surface.levels == 5 && !texture.copiedResized,
		"a copy of the creation size goes into the created surface, its mips kept" );
	Check( texture.uploads.empty(), "a copy forgets the recorded uploads: level 0 is no upload now" );
	Check( texture.sampler == D3D12_PackSampler( { samplerFilter_t::LINEAR, samplerAddress_t::CLAMP, 4.0f, -0.5f } ),
		"a colour copy sets LINEAR and CLAMP and keeps the anisotropy and the LOD bias, as GL's copy" );

	const d3d12TextureSurface_t * big = store.PrepareCopyTarget( color, d3d12CopyKind_t::COLOR, 1280, 720 );
	Check( big != nullptr && big->width == 1280 && big->height == 720 && big->levels == 1, "another size: a new one-level surface" );
	Check( texture.spare.resource.Get() == created && store.CopySurfacesThisFrame() == 1, "the created surface is the spare" );
	Check( texture.copiedResized && !texture.IncompleteCopy() && store.SampledView( color, textureKind_t::TEX_2D ) == texture.surface.srv,
		"a resized copy under LINEAR is complete: it samples its surface" );
	store.SetSampler( color, trilinear );
	Check( texture.IncompleteCopy() && store.SampledView( color, textureKind_t::TEX_2D ) == store.Fallback( textureKind_t::TEX_2D ),
		"a mipmap filter on a resized copy: GL's incomplete texture reads black (T6)" );
	store.SetSampler( color, { samplerFilter_t::LINEAR_MIP_NEAREST, samplerAddress_t::CLAMP, 1.0f, 0.0f } );
	Check( texture.IncompleteCopy(), "LINEAR_MIP_NEAREST reads the mips too" );
	store.SetSampler( color, POINT_CLAMP );
	Check( !texture.IncompleteCopy(), "NEAREST reads level 0 only" );

	const ID3D12Resource * bigResource = texture.surface.resource.Get();
	Check( store.PrepareCopyTarget( color, d3d12CopyKind_t::COLOR, 960, 540 ) != nullptr && texture.spare.resource.Get() == bigResource
		&& store.CopySurfacesThisFrame() == 2, "a third size: a new surface, the last one is the spare and the created one is buried" );
	const ID3D12Resource * smallResource = texture.surface.resource.Get();
	for ( int i = 0; i < 3; i++ ) {
		static_cast<void>( store.PrepareCopyTarget( color, d3d12CopyKind_t::COLOR, 1280, 720 ) );
		Check( texture.surface.resource.Get() == bigResource && texture.spare.resource.Get() == smallResource, "the spare swaps in" );
		static_cast<void>( store.PrepareCopyTarget( color, d3d12CopyKind_t::COLOR, 960, 540 ) );
		Check( texture.surface.resource.Get() == smallResource && texture.spare.resource.Get() == bigResource, "and back" );
	}
	Check( store.CopySurfacesThisFrame() == 2, "resolution scaling between two sizes creates nothing (P1)" );
	Check( store.Bytes() == bytesBefore + texture.surface.bytes + texture.spare.bytes + store.Get( depth ).surface.bytes,
		"the bytes count both surfaces, not the buried one" );
	static_cast<void>( store.PrepareCopyTarget( color, d3d12CopyKind_t::COLOR, 16, 16 ) );
	Check( texture.surface.levels == 1 && !texture.copiedResized, "the creation size again, after a third size: a one-level surface, complete" );
	store.ResetFrameCounters();
	Check( store.CopySurfacesThisFrame() == 0, "the frame counter restarts" );

	const d3d12TextureSurface_t * depthTarget = store.PrepareCopyTarget( depth, d3d12CopyKind_t::DEPTH, 32, 24 );
	Check( depthTarget != nullptr && depthTarget->rtv.IsValid() && depthTarget->width == 32 && depthTarget->height == 24,
		"a depth copy target has an RTV for the depth-copy pass" );
	Check( store.Get( depth ).sampler == D3D12_PackSampler( POINT_CLAMP ), "a depth copy keeps the sampler, as GL's copy" );
	printf( "  three warnings follow: copies into the wrong kind of texture\n" );
	Check( store.PrepareCopyTarget( depth, d3d12CopyKind_t::COLOR, 8, 8 ) == nullptr, "a depth texture is no colour copy target" );
	Check( store.PrepareCopyTarget( color, d3d12CopyKind_t::DEPTH, 8, 8 ) == nullptr, "a colour texture is no depth copy target" );
	const textureHandle_t cube = store.Create( Opts( FMT_RGBA8, 4, 4, 1, TT_CUBIC ), textureRole_t::COLOR_COPY, POINT_CLAMP, "cube" );
	Check( store.PrepareCopyTarget( cube, d3d12CopyKind_t::COLOR, 4, 4 ) == nullptr, "a cube is no copy target" );
	Check( store.PrepareCopyTarget( textureHandle_t::NONE, d3d12CopyKind_t::COLOR, 8, 8 ) == nullptr, "NONE" );
	Check( store.PrepareCopyTarget( color, d3d12CopyKind_t::COLOR, 0, 8 ) == nullptr, "an empty box" );

	store.Destroy( color );
	store.Destroy( depth );
	store.Destroy( cube );
	Check( store.Bytes() == bytesBefore, "Destroy buries both surfaces" );
	gpu->WaitIdle();
	CheckNoLayerErrors( *gpu );
}

// The context's copy path over a test GPU: a SIZE x SIZE scene target, the internal passes and the screen
// copies. The context's List() gives a new list the heaps and the root signature; here Bind() does, before a pass.
class idTestCopies {
public:
	static constexpr int			SIZE = 64;
	struct box_t {
		int							x, y, width, height;
	};
	// inside, the whole scene, past the right and top, past the left and bottom (GL's coordinates)
	static constexpr std::array<box_t, 4>	BOXES = { { { 5, 3, 20, 11 }, { 0, 0, SIZE, SIZE }, { 50, 40, 30, 30 }, { -6, -2, 16, 12 } } };

									idTestCopies( idTestGpu & gpu, UINT samples );
									~idTestCopies();
									idTestCopies( const idTestCopies & ) = delete;
	idTestCopies &					operator=( const idTestCopies & ) = delete;
	[[nodiscard]] bool				Ready() const { return ready; }
	void							Bind();
	// The scene colour: a different RGBA per pixel with one sample (an upload), per 4 x 4 block with MSAA (clears).
	void							FillColor();
	[[nodiscard]] uint32			ColorAt( int column, int row ) const;	// what FillColor wrote, R | G << 8 | B << 16 | A << 24
	// The scene depth: a different value per 4 x 4 block (clears).
	void							FillDepth();
	[[nodiscard]] static float		DepthAt( int column, int row );
	// Level 0 of a single-sample texture as 32-bit texels, rows in memory order.
	[[nodiscard]] std::vector<uint32>	Read( ID3D12Resource & resource, d3d12ResourceState_t & state );
	[[nodiscard]] static bool		InScene( int column, int row ) { return column >= 0 && row >= 0 && column < SIZE && row < SIZE; }

	idTestGpu &						gpu;
	UINT							samples;
	idD3D12HlslCompiler				hlslCompiler;
	idD3D12RootSignature			root;
	idD3D12DescriptorPool			dsvPool;
	idD3D12UploadRing				constants;
	idD3D12SceneTarget				scene;
	idD3D12InternalPasses			passes;
	idD3D12ScreenCopies				copies;
	idD3D12Readback					readback;

private:
	bool							ready = false;
};

idTestCopies::idTestCopies( idTestGpu & owner, UINT sampleCount ) :
	gpu( owner ),
	samples( sampleCount ),
	hlslCompiler( owner.library, RepoReader(), false ),
	root( owner.device, owner.library ),
	dsvPool( owner.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV ),
	constants( owner.device, 1 << 20, L"test constants ring" ),
	scene( owner.device, owner.graveyard, owner.srvPool, owner.rtvPool, dsvPool ),
	passes( owner.device, root, hlslCompiler, owner, owner.barriers, owner.uploader, constants, owner.samplers, owner.srvPool ),
	copies( owner, owner.barriers, scene, owner.textures, passes, owner.rtvPool ),
	readback( owner.device, owner, owner.barriers, owner.graveyard ) {
	if ( !gpu.textures.Fallback( textureKind_t::TEX_2D ).IsValid() ) {
		gpu.textures.CreateDefaults();
	}
	if ( !root.Get() ) {
		Check( false, "the root signature" );
		return;
	}
	scene.Create( SIZE, SIZE, samples );
	passes.CreateDefaults();
	ready = Check( scene.Color().resource && scene.Depth().resource, va( "a %u-sample scene target", samples ) );
}

idTestCopies::~idTestCopies() {
	if ( !gpu.device.IsLost() ) {
		gpu.WaitIdle();
	}
}

void idTestCopies::Bind() {
	ID3D12GraphicsCommandList1 & list = gpu.List();
	ID3D12DescriptorHeap * heaps[] = { gpu.srvPool.ShaderVisibleHeap(), gpu.samplers.Heap() };
	list.SetDescriptorHeaps( 2, heaps );
	list.SetGraphicsRootSignature( root.Get() );
}

[[nodiscard]] uint32 ScenePattern( int column, int row ) {
	const uint32 r = static_cast<uint32>( column * 4 );
	const uint32 g = static_cast<uint32>( row * 4 );
	const uint32 b = static_cast<uint32>( ( column * 7 + row * 3 ) & 0xFF );
	const uint32 a = static_cast<uint32>( ( 255 - column - row * 2 ) & 0xFF );
	return r | ( g << 8 ) | ( b << 16 ) | ( a << 24 );
}

uint32 idTestCopies::ColorAt( int column, int row ) const {
	return ( samples == 1 ) ? ScenePattern( column, row ) : ScenePattern( column & ~3, row & ~3 );
}

float idTestCopies::DepthAt( int column, int row ) {
	return static_cast<float>( ( row / 4 ) * ( SIZE / 4 ) + column / 4 + 1 ) / 257.0f;
}

// A multisampled texture takes no copy from a buffer, and a clear sets every sample.
void idTestCopies::FillColor() {
	idD3D12SceneTarget::surface_t & color = scene.Color();
	if ( samples == 1 ) {
		const D3D12_RESOURCE_DESC desc = color.resource->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed = {};
		UINT rows = 0;
		UINT64 total = 0;
		gpu.device.Get().GetCopyableFootprints( &desc, 0, 1, 0, &placed, &rows, nullptr, &total );
		const std::optional<d3d12UploadSpan_t> span = gpu.uploader.Stage( total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT );
		if ( !Check( span.has_value(), "staging for the scene pattern" ) ) {
			return;
		}
		for ( int row = 0; row < SIZE; row++ ) {
			for ( int column = 0; column < SIZE; column++ ) {
				const uint32 texel = ScenePattern( column, row );
				memcpy( span->cpu + static_cast<size_t>( row ) * placed.Footprint.RowPitch + column * 4, &texel, 4 );
			}
		}
		gpu.uploader.TextureRegion( *color.resource.Get(), color.state, 0, *span, placed.Footprint, rows );
		return;
	}
	gpu.barriers.Transition( *color.resource.Get(), color.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	gpu.barriers.Flush();
	for ( int row = 0; row < SIZE; row += 4 ) {
		for ( int column = 0; column < SIZE; column += 4 ) {
			const uint32 texel = ScenePattern( column, row );
			const float rgba[4] = { Unorm8( static_cast<byte>( texel ) ), Unorm8( static_cast<byte>( texel >> 8 ) ), Unorm8( static_cast<byte>( texel >> 16 ) ),
				Unorm8( static_cast<byte>( texel >> 24 ) ) };
			const D3D12_RECT rect = { column, row, column + 4, row + 4 };
			gpu.List().ClearRenderTargetView( scene.Rtv( false ), rgba, 1, &rect );
		}
	}
}

void idTestCopies::FillDepth() {
	idD3D12SceneTarget::surface_t & depth = scene.Depth();
	gpu.barriers.Transition( *depth.resource.Get(), depth.state, D3D12_RESOURCE_STATE_DEPTH_WRITE );
	gpu.barriers.Flush();
	for ( int row = 0; row < SIZE; row += 4 ) {
		for ( int column = 0; column < SIZE; column += 4 ) {
			const D3D12_RECT rect = { column, row, column + 4, row + 4 };
			gpu.List().ClearDepthStencilView( scene.Dsv(), D3D12_CLEAR_FLAG_DEPTH, DepthAt( column, row ), 0, 1, &rect );
		}
	}
}

// Level 0 of a single-sample texture as 32-bit texels, rows in memory order; waits for the GPU.
// Level 0 as texels of sizeof( T ) bytes, rows in memory order.
template<typename T = uint32>
[[nodiscard]] std::vector<T> ReadTexels( idTestGpu & gpu, ID3D12Resource & resource, d3d12ResourceState_t & state ) {
	const D3D12_RESOURCE_DESC desc = resource.GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT rows = 0;
	UINT64 total = 0;
	gpu.device.Get().GetCopyableFootprints( &desc, 0, 1, 0, &footprint, &rows, nullptr, &total );
	const idComPtr<ID3D12Resource> buffer = D3D12_CreateBuffer( gpu.device, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST,
		L"test copy readback" );
	if ( !Check( buffer != nullptr, "the readback buffer" ) ) {
		return {};
	}
	gpu.barriers.Transition( resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE );
	gpu.barriers.Flush();
	D3D12_TEXTURE_COPY_LOCATION destination = {};
	destination.pResource = buffer.Get();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint = footprint;
	D3D12_TEXTURE_COPY_LOCATION source = {};
	source.pResource = &resource;
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	gpu.List().CopyTextureRegion( &destination, 0, 0, 0, &source, nullptr );
	gpu.WaitIdle();

	const size_t width = static_cast<size_t>( desc.Width );
	std::vector<T> texels( width * rows );
	const D3D12_RANGE everything = { 0, static_cast<SIZE_T>( total ) };
	void * mapped = nullptr;
	if ( gpu.device.Check( buffer->Map( 0, &everything, &mapped ), "Map (test copy readback)" ) ) {
		for ( UINT row = 0; row < rows; row++ ) {
			memcpy( &texels[row * width], static_cast<const byte *>( mapped ) + static_cast<size_t>( row ) * footprint.Footprint.RowPitch, width * sizeof( T ) );
		}
		const D3D12_RANGE nothingWritten = { 0, 0 };
		buffer->Unmap( 0, &nothingWritten );
	}
	return texels;
}

std::vector<uint32> idTestCopies::Read( ID3D12Resource & resource, d3d12ResourceState_t & state ) {
	return ReadTexels( gpu, resource, state );
}

// Every box into one texture, which takes the box's size: texel (c, r) is the scene pixel (x + c, y + r),
// GL's row order, and the same as idD3D12Readback reads from the (resolved) scene. Texels off the scene are undefined.
void CopyColorBoxes( idTestGpu & gpu, UINT samples ) {
	idTestCopies fixture( gpu, samples );
	if ( !fixture.Ready() ) {
		return;
	}
	const textureHandle_t handle = gpu.textures.Create( Opts( FMT_RGBA8, 16, 16, 5 ), textureRole_t::COLOR_COPY,
		{ samplerFilter_t::LINEAR_MIP_LINEAR, samplerAddress_t::REPEAT, 1.0f, 0.0f }, "_currentRender" );
	if ( !Check( handle != textureHandle_t::NONE, "Create" ) ) {
		return;
	}
	fixture.FillColor();
	for ( const idTestCopies::box_t & box : idTestCopies::BOXES ) {
		fixture.copies.Color( handle, box.x, box.y, box.width, box.height );
		d3d12TextureSurface_t & surface = gpu.textures.Get( handle ).surface;
		const std::vector<uint32> texels = fixture.Read( *surface.resource.Get(), surface.state );
		std::vector<byte> rgb( static_cast<size_t>( box.width ) * box.height * 3 );
		idD3D12SceneTarget::surface_t & resolved = fixture.scene.Resolved( gpu.barriers, gpu );
		fixture.readback.Read( *resolved.resource.Get(), resolved.state, box.x, box.y, box.width, box.height, rgb.data(), false );
		if ( !Check( surface.width == box.width && surface.height == box.height && texels.size() == rgb.size() / 3,
				va( "the texture is the box's %d x %d", box.width, box.height ) ) ) {
			continue;
		}
		int wrong = 0;
		int unlikeReadback = 0;
		int compared = 0;
		for ( int row = 0; row < box.height; row++ ) {
			for ( int column = 0; column < box.width; column++ ) {
				if ( !idTestCopies::InScene( box.x + column, box.y + row ) ) {
					continue;
				}
				const size_t at = static_cast<size_t>( row ) * box.width + column;
				const uint32 texel = texels[at];
				wrong += ( texel != fixture.ColorAt( box.x + column, box.y + row ) ) ? 1 : 0;
				const uint32 read = rgb[at * 3] | ( rgb[at * 3 + 1] << 8 ) | ( rgb[at * 3 + 2] << 16 );
				unlikeReadback += ( ( texel & 0xFFFFFF ) != read ) ? 1 : 0;
				compared++;
			}
		}
		Check( compared > 0 && wrong == 0, va( "box %d %d %d x %d: %d of %d texels are not the scene pixel of their GL row", box.x, box.y, box.width,
			box.height, wrong, compared ) );
		Check( unlikeReadback == 0, va( "box %d %d %d x %d: %d texels unlike the readback", box.x, box.y, box.width, box.height, unlikeReadback ) );
	}
	const idD3D12ScreenCopies::stats_t stats = fixture.copies.TakeStats();
	Check( stats.color == static_cast<int>( idTestCopies::BOXES.size() ) && stats.depth == 0, "the copies are counted" );
	gpu.textures.Destroy( handle );
	gpu.WaitIdle();
	CheckNoLayerErrors( gpu );
}

// The depth-copy pass: texel (c, r) holds the depth of scene pixel (x + c, y + r) as the float of its 24 bits,
// sample 0 under MSAA; 0 off the scene. With one sample the 24 bits are read back as well.
void CopyDepthBoxes( idTestGpu & gpu, UINT samples ) {
	idTestCopies fixture( gpu, samples );
	if ( !fixture.Ready() ) {
		return;
	}
	const textureHandle_t handle = gpu.textures.Create( Opts( FMT_DEPTH, 16, 16, 1 ), textureRole_t::DEPTH_COPY, POINT_CLAMP, "_currentDepth" );
	if ( !Check( handle != textureHandle_t::NONE, "Create" ) ) {
		return;
	}
	fixture.FillDepth();
	std::vector<uint32> written;
	if ( samples == 1 ) {
		idD3D12SceneTarget::surface_t & depth = fixture.scene.Depth();
		written = fixture.Read( *depth.resource.Get(), depth.state );
		for ( uint32 & value : written ) {
			value &= 0xFFFFFFu;
		}
	}
	constexpr double STEPS = 16777215.0;
	double maxSteps = 0.0;
	for ( const idTestCopies::box_t & box : idTestCopies::BOXES ) {
		fixture.Bind();
		fixture.copies.Depth( handle, box.x, box.y, box.width, box.height );
		d3d12TextureSurface_t & surface = gpu.textures.Get( handle ).surface;
		const std::vector<uint32> texels = fixture.Read( *surface.resource.Get(), surface.state );
		if ( !Check( surface.width == box.width && surface.height == box.height && texels.size() == static_cast<size_t>( box.width ) * box.height,
				va( "the texture is the box's %d x %d", box.width, box.height ) ) ) {
			continue;
		}
		int wrong = 0;
		int unlikeWritten = 0;
		int offScene = 0;
		for ( int row = 0; row < box.height; row++ ) {
			for ( int column = 0; column < box.width; column++ ) {
				const int px = box.x + column;
				const int py = box.y + row;
				const float value = std::bit_cast<float>( texels[static_cast<size_t>( row ) * box.width + column] );
				if ( !idTestCopies::InScene( px, py ) ) {
					offScene += ( value != 0.0f ) ? 1 : 0;
					continue;
				}
				wrong += ( std::fabs( static_cast<double>( value ) - idTestCopies::DepthAt( px, py ) ) > 1.0 / STEPS ) ? 1 : 0;
				if ( !written.empty() ) {
					// a float holds 24 bits near 1 only to half a step, and AMD converts R24 to float within one ulp
					const double steps = std::fabs( value * STEPS - written[static_cast<size_t>( py ) * idTestCopies::SIZE + px] );
					maxSteps = Max( maxSteps, steps );
					unlikeWritten += ( steps > 1.0 ) ? 1 : 0;
				}
			}
		}
		Check( wrong == 0, va( "box %d %d %d x %d: %d texels are not the depth of their GL row", box.x, box.y, box.width, box.height, wrong ) );
		Check( unlikeWritten == 0, va( "box %d %d %d x %d: %d texels are not the 24 bits read back", box.x, box.y, box.width, box.height, unlikeWritten ) );
		Check( offScene == 0, va( "box %d %d %d x %d: %d texels off the scene are not 0", box.x, box.y, box.width, box.height, offScene ) );
	}
	if ( !written.empty() ) {
		printf( "  the copy is at most %.3f D24 steps from the 24 bits read back\n", maxSteps );
	}

	// shaders read the copy as R R R 1, like GL's depth texture
	idTestSampler sampler( gpu );
	if ( sampler.Ready() ) {
		gpu.textures.MakeReadable( handle );
		const idTestCopies::box_t & last = idTestCopies::BOXES.back();
		const std::array<idVec4, 1> centre = { idVec4( ( 8 + 0.5f ) / last.width, ( 5 + 0.5f ) / last.height, 0.0f, 0.0f ) };
		const std::vector<idVec4> sampled = sampler.Sample( gpu.textures.SampledView( handle, textureKind_t::TEX_2D ), textureKind_t::TEX_2D,
			SamplerOf( samplerFilter_t::NEAREST, samplerAddress_t::CLAMP ), centre );
		const float expected = idTestCopies::DepthAt( last.x + 8, last.y + 5 );
		Check( !sampled.empty() && Near( sampled[0], idVec4( expected, expected, expected, 1.0f ), 1.5f / 16777215.0f ),
			va( "sampled %s, depth %.7f", sampled.empty() ? "nothing" : Text( sampled[0] ), expected ) );
	}
	gpu.textures.Destroy( handle );
	gpu.WaitIdle();
	CheckNoLayerErrors( gpu );
}

void CopyColor1( idTestGpu & gpu ) {
	CopyColorBoxes( gpu, 1 );
}

void CopyColor4( idTestGpu & gpu ) {
	CopyColorBoxes( gpu, 4 );
}

void CopyDepth1( idTestGpu & gpu ) {
	CopyDepthBoxes( gpu, 1 );
}

void CopyDepth4( idTestGpu & gpu ) {
	CopyDepthBoxes( gpu, 4 );
}

// The path of d3d12_removeDevice on WARP with DRED (debug level 3): work in flight, RemoveDevice, every wait
// returns, the next failing call reports the loss, and the device stays lost. The runtime completes the lists of
// a device the application removes, so DRED has no unfinished one here; only a real GPU fault leaves one.
void Test_DeviceLoss() {
	auto library = idD3D12Library::Load();
	if ( !Check( library.has_value(), "idD3D12Library::Load" ) ) {
		return;
	}
	auto created = idD3D12Device::Create( *library, "warp", 3 );
	if ( !Check( created.has_value(), "idD3D12Device::Create( warp, 3 )" ) ) {
		return;
	}
	idTestGpu gpu( std::move( *library ), std::move( *created ), 4 << 20, d3d12BufferPlacement_t::UPLOAD );
	constexpr uint64 BYTES = 16 << 20;
	idComPtr<ID3D12Resource> from = D3D12_CreateBuffer( gpu.device, D3D12_HEAP_TYPE_DEFAULT, BYTES, D3D12_RESOURCE_STATE_COMMON, L"test loss from" );
	idComPtr<ID3D12Resource> to = D3D12_CreateBuffer( gpu.device, D3D12_HEAP_TYPE_DEFAULT, BYTES, D3D12_RESOURCE_STATE_COMMON, L"test loss to" );
	if ( !Check( from && to, "the buffers" ) ) {
		return;
	}
	for ( int i = 0; i < 16; i++ ) {
		gpu.List().CopyBufferRegion( to.Get(), 0, from.Get(), 0, BYTES );
	}
	const uint64 fence = gpu.SubmitChunk();
	Check( !gpu.device.IsLost(), "the device works before the removal" );
	gpu.device.RemoveDevice();
	gpu.WaitFor( fence );	// returns: a removed device completes every fence
	Check( gpu.queue.Waited() >= fence, "the wait for the work in flight returned" );
	Check( !gpu.device.Check( gpu.device.Get().GetDeviceRemovedReason(), "GetDeviceRemovedReason (test)" ), "the removed reason is a failure" );
	Check( gpu.device.IsLost(), "the device is lost" );
	gpu.WaitFor( gpu.SubmitChunk() );
	Check( gpu.device.IsLost(), "a submit and a wait after the loss return, the device stays lost" );
}

// The line ReportDred prints for a list: the names, the progress and the operation the GPU stopped before.
void Test_BreadcrumbText() {
	const D3D12_AUTO_BREADCRUMB_OP history[] = { D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT, D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED,
		D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER, D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED, D3D12_AUTO_BREADCRUMB_OP_ENDEVENT };
	UINT done = 2;
	D3D12_AUTO_BREADCRUMB_NODE node = {};
	node.pCommandListDebugNameA = "doom3 frame list";
	node.pCommandQueueDebugNameA = "doom3 direct queue";
	node.pLastBreadcrumbValue = &done;
	node.pCommandHistory = history;
	node.BreadcrumbCount = static_cast<UINT>( std::size( history ) );
	const idStr stopped = D3D12_BreadcrumbText( node );
	Check( stopped == "list doom3 frame list on doom3 direct queue: 2 of 5 operations done, stopped before ResourceBarrier", stopped.c_str() );
	done = node.BreadcrumbCount;
	node.pCommandListDebugNameA = nullptr;
	const idStr finished = D3D12_BreadcrumbText( node );
	Check( finished == "list (unnamed) on doom3 direct queue: 5 of 5 operations done, the last EndEvent", finished.c_str() );
	const D3D12_AUTO_BREADCRUMB_OP later[] = { static_cast<D3D12_AUTO_BREADCRUMB_OP>( 48 ) };
	node.pCommandHistory = later;
	node.BreadcrumbCount = 1;
	done = 0;
	const idStr unknown = D3D12_BreadcrumbText( node );
	Check( unknown == "list (unnamed) on doom3 direct queue: 0 of 1 operations done, stopped before operation 48", unknown.c_str() );
}

// R_SetColorMappings' ramp, in its float arithmetic.
[[nodiscard]] d3d12GammaRamp_t ColorMappings( float gamma, float brightness ) {
	d3d12GammaRamp_t ramp;
	const float invGamma = 1.0f / gamma;
	float j = 0.0f;
	for ( int i = 0; i < 256; i++, j += brightness ) {
		const int value = idMath::ClampInt( 0, 0xFFFF, idMath::Ftoi( 0xffff * pow( j / 255.0f, invGamma ) + 0.5f ) );
		ramp.red[i] = ramp.green[i] = ramp.blue[i] = static_cast<uint16>( value );
	}
	return ramp;
}

void Test_GammaIdentity() {
	Check( D3D12_GammaIsIdentity( D3D12_IdentityGamma() ), "i * 257 is the identity" );
	Check( D3D12_GammaIsIdentity( ColorMappings( 1.0f, 1.0f ) ), "r_gamma 1, r_brightness 1 (the defaults) build the identity" );
	Check( !D3D12_GammaIsIdentity( ColorMappings( 1.5f, 1.0f ) ), "r_gamma 1.5 is no identity" );
	Check( !D3D12_GammaIsIdentity( ColorMappings( 1.0f, 1.2f ) ), "r_brightness 1.2 is no identity" );
	d3d12GammaRamp_t ramp = D3D12_IdentityGamma();
	ramp.green[100] = static_cast<uint16>( 100 * 257 + 128 );
	Check( D3D12_GammaIsIdentity( ramp ), "a value that still rounds to its byte keeps the identity" );
	ramp.green[100] = static_cast<uint16>( 100 * 257 + 129 );
	Check( !D3D12_GammaIsIdentity( ramp ), "one value that rounds to the next byte, in one channel, is no identity" );
}

void Test_SceneSamples() {
	constexpr std::array<bool, 4> ALL = { true, true, true, true };
	constexpr std::array<bool, 4> NO_8 = { true, true, true, false };
	constexpr std::array<bool, 4> ONLY_1 = { true, false, false, false };
	constexpr std::array<bool, 4> NO_2 = { true, false, true, true };
	const struct {
		int							requested;
		const std::array<bool, 4> *	supported;
		UINT						samples;
	} CASES[] = { { 0, &ALL, 1 }, { 1, &ALL, 1 }, { -4, &ALL, 1 }, { 2, &ALL, 2 }, { 3, &ALL, 2 }, { 4, &ALL, 4 }, { 8, &ALL, 8 }, { 16, &ALL, 8 },
		{ 8, &NO_8, 4 }, { 4, &ONLY_1, 1 }, { 2, &NO_2, 1 }, { 4, &NO_2, 4 } };
	for ( const auto & c : CASES ) {
		const UINT samples = D3D12_SceneSamples( c.requested, *c.supported );
		Check( samples == c.samples, va( "r_multiSamples %d: %u samples, expected %u", c.requested, samples, c.samples ) );
	}
}

// The present blit into a stand-in back buffer: rows flipped, the bytes exact under an identity ramp, each colour
// channel through its LUT under r_gamma 1.5 and alpha kept. With MSAA it reads the resolved scene.
void PresentBlitRamps( idTestGpu & gpu, UINT samples ) {
	idTestCopies fixture( gpu, samples );
	if ( !fixture.Ready() ) {
		return;
	}
	constexpr int SIZE = idTestCopies::SIZE;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = SIZE;
	desc.Height = SIZE;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = idD3D12SwapChain::FORMAT;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	idComPtr<ID3D12Resource> back;
	if ( !Check( SUCCEEDED( gpu.device.Get().CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
			IID_PPV_ARGS( &back ) ) ), "the stand-in back buffer" ) ) {
		return;
	}
	d3d12ResourceState_t backState;
	backState.current = D3D12_RESOURCE_STATE_RENDER_TARGET;
	const d3d12Descriptor_t rtv = gpu.rtvPool.Allocate();
	gpu.device.Get().CreateRenderTargetView( back.Get(), nullptr, gpu.rtvPool.Cpu( rtv ) );
	fixture.FillColor();

	const d3d12GammaRamp_t identity = ColorMappings( 1.0f, 1.0f );
	const d3d12GammaRamp_t gamma = ColorMappings( 1.5f, 1.0f );
	const struct {
		const char *				name;
		const d3d12GammaRamp_t *	ramp;
	} RAMPS[] = { { "identity", &identity }, { "r_gamma 1.5", &gamma }, { "identity again", &identity } };
	for ( const auto & step : RAMPS ) {
		fixture.passes.SetGamma( *step.ramp );
		Check( fixture.passes.GammaIsIdentity() == ( step.ramp == &identity ), va( "%s: GammaIsIdentity", step.name ) );
		idD3D12SceneTarget::surface_t & source = fixture.scene.Resolved( gpu.barriers, gpu );
		gpu.barriers.Transition( *source.resource.Get(), source.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		gpu.barriers.Transition( *back.Get(), backState, D3D12_RESOURCE_STATE_RENDER_TARGET );
		fixture.Bind();
		fixture.passes.PresentBlit( source.srv, gpu.rtvPool.Cpu( rtv ), SIZE, SIZE );
		const std::vector<uint32> texels = fixture.Read( *back.Get(), backState );
		if ( !Check( texels.size() == static_cast<size_t>( SIZE ) * SIZE, va( "%s: the back buffer is read", step.name ) ) ) {
			continue;
		}
		int wrong = 0;
		int offByOne = 0;
		for ( int row = 0; row < SIZE; row++ ) {
			for ( int column = 0; column < SIZE; column++ ) {
				const uint32 scene = fixture.ColorAt( column, SIZE - 1 - row );	// back buffer row 0 is the top of the screen
				const uint32 shown = texels[static_cast<size_t>( row ) * SIZE + column];
				for ( int channel = 0; channel < 4; channel++ ) {
					const uint32 in = ( scene >> ( channel * 8 ) ) & 0xFF;
					const std::array<uint16, 256> & lut = ( channel == 0 ) ? step.ramp->red : ( ( channel == 1 ) ? step.ramp->green : step.ramp->blue );
					const uint32 expected = ( channel == 3 ) ? in : ( lut[in] * 255u + 32767u ) / 65535u;
					const int delta = std::abs( static_cast<int>( ( shown >> ( channel * 8 ) ) & 0xFF ) - static_cast<int>( expected ) );
					wrong += ( delta > 1 || ( delta == 1 && step.ramp == &identity ) ) ? 1 : 0;
					offByOne += ( delta == 1 ) ? 1 : 0;
				}
			}
		}
		Check( wrong == 0, va( "%s, %u sample(s): %d channels unlike the scene through the ramp (%d off by one)", step.name, samples, wrong, offByOne ) );
	}
	gpu.WaitIdle();
	gpu.rtvPool.Free( rtv );
	CheckNoLayerErrors( gpu );
}

void PresentBlit1( idTestGpu & gpu ) {
	PresentBlitRamps( gpu, 1 );
}

void PresentBlit4( idTestGpu & gpu ) {
	PresentBlitRamps( gpu, 4 );
}

// ---- the draw recorder --------------------------------------------------------------------------------------

// The recorder over a test GPU, on a SIZE x SIZE scene target, with a buffer store of its own in the FRAME placement
// under test. The context's List() gives a new list the heaps, the root signature and an empty bound state; here NewList() does.
class idTestRecorder {
public:
	static constexpr int			SIZE = 64;

									idTestRecorder( idTestGpu & gpu, d3d12BufferPlacement_t framePlacement );
									~idTestRecorder();
									idTestRecorder( const idTestRecorder & ) = delete;
	idTestRecorder &				operator=( const idTestRecorder & ) = delete;
	[[nodiscard]] bool				Ready() const { return ready; }
	// Both stages of base/shaders/<name>.hlsl (or its SKINNED variant), linked into the library; NONE when they do not build.
	[[nodiscard]] programHandle_t	Program( const char * name );
	void							NewList();
	void							NextList() { gpu.SubmitChunk(); NewList(); }	// a chunk boundary, as r_d3d12SubmitDraws makes one
	[[nodiscard]] std::vector<uint32>	ReadColor();	// ends the pass and waits; rows in GL order

	idTestGpu &						gpu;
	idD3D12HlslCompiler				hlslCompiler;
	idD3D12RootSignature			root;
	idD3D12DescriptorPool			dsvPool;
	idD3D12UploadRing				constants;
	idD3D12BufferStore				buffers;
	idD3D12ShaderLibrary			shaders;
	idD3D12PipelineFactory			factory;
	idD3D12SceneTarget				scene;
	idD3D12InternalPasses			passes;
	idD3D12DrawRecorder				recorder;

private:
	bool							ready = false;
};

idTestRecorder::idTestRecorder( idTestGpu & owner, d3d12BufferPlacement_t framePlacement ) :
	gpu( owner ),
	hlslCompiler( owner.library, RepoReader(), false ),
	root( owner.device, owner.library ),
	dsvPool( owner.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV ),
	constants( owner.device, 1 << 20, L"test constants ring" ),
	buffers( owner.device, owner.graveyard, owner.uploader, owner.barriers, framePlacement ),
	shaders( hlslCompiler, owner.graveyard ),
	factory( owner.device, root, nullptr, nullptr ),
	scene( owner.device, owner.graveyard, owner.srvPool, owner.rtvPool, dsvPool ),
	passes( owner.device, root, hlslCompiler, owner, owner.barriers, owner.uploader, constants, owner.samplers, owner.srvPool ),
	recorder( owner.device, owner, owner.barriers, scene, passes, factory, shaders, buffers, owner.textures, constants, owner.samplers ) {
	if ( !gpu.textures.Fallback( textureKind_t::TEX_2D ).IsValid() ) {
		gpu.textures.CreateDefaults();
	}
	if ( !root.Get() ) {
		Check( false, "the root signature" );
		return;
	}
	buffers.CreateDefaults();
	scene.Create( SIZE, SIZE, 1 );
	passes.CreateDefaults();
	ready = Check( scene.Color().resource && scene.Depth().resource && buffers.ZeroJoints() != 0, "the scene target and the zero joints" );
}

idTestRecorder::~idTestRecorder() {
	if ( !gpu.device.IsLost() ) {
		gpu.WaitIdle();
	}
	buffers.DestroyAll();
	shaders.DestroyAll();
}

programHandle_t idTestRecorder::Program( const char * name ) {
	const auto vertex = RepoStage( hlslCompiler, name, shaderStageType_t::VERTEX );
	const auto fragment = RepoStage( hlslCompiler, name, shaderStageType_t::FRAGMENT );
	if ( !Check( vertex && fragment, va( "%s builds", name ) ) ) {
		return programHandle_t::NONE;
	}
	auto program = D3D12_LinkProgram( *vertex, &*fragment );
	if ( !Check( program.has_value(), va( "%s links", name ) ) ) {
		return programHandle_t::NONE;
	}
	return shaders.AddProgram( std::move( *program ) );
}

void idTestRecorder::NewList() {
	ID3D12GraphicsCommandList1 & list = gpu.List();
	ID3D12DescriptorHeap * heaps[] = { gpu.srvPool.ShaderVisibleHeap(), gpu.samplers.Heap() };
	list.SetDescriptorHeaps( 2, heaps );
	list.SetGraphicsRootSignature( root.Get() );
	recorder.NewList();
}

std::vector<uint32> idTestRecorder::ReadColor() {
	recorder.EndPass();
	idD3D12SceneTarget::surface_t & color = scene.Color();
	return ReadTexels( gpu, *color.resource.Get(), color.state );
}

// A skinned quad per list from a FRAME set (vertices, indexes, joints) in one placement. A STAGED buffer decays to
// COMMON with every list, so it transitions again in the next one, before the root CBV of its joints is set (the
// layer's #527 otherwise). Joint set 0 moves the quad left, set 1 right; joints read as zeros would draw nothing.
void DrawFrameBuffers( idTestGpu & gpu, d3d12BufferPlacement_t placement ) {
	constexpr int SIZE = idTestRecorder::SIZE;
	constexpr float HALF_WIDTH = 0.45f;
	constexpr float HALF_HEIGHT = 0.9f;
	constexpr std::array<float, 2> SHIFT = { -0.5f, 0.5f };
	constexpr int SET_BYTES = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;	// jointOffset steps
	constexpr int SET_VECTORS = SET_BYTES / static_cast<int>( sizeof( idVec4 ) );

	idTestRecorder fixture( gpu, placement );
	if ( !fixture.Ready() ) {
		return;
	}
	const programHandle_t program = fixture.Program( "depth_skinned" );
	const d3d12Program_t * linked = fixture.shaders.FindProgram( program );
	if ( !Check( linked != nullptr && linked->info.usesJoints, "depth_skinned reads joints" ) ) {
		return;
	}

	std::array<idDrawVert, 4> verts;
	constexpr float CORNERS[4][2] = { { -HALF_WIDTH, -HALF_HEIGHT }, { HALF_WIDTH, -HALF_HEIGHT }, { HALF_WIDTH, HALF_HEIGHT }, { -HALF_WIDTH, HALF_HEIGHT } };
	for ( size_t i = 0; i < verts.size(); i++ ) {
		verts[i].Clear();
		verts[i].xyz.Set( CORNERS[i][0], CORNERS[i][1], 0.0f );
		verts[i].color2[0] = 255;		// joint 0 (color) with weight 1
	}
	constexpr std::array<triIndex_t, 6> INDEXES = { 0, 1, 2, 0, 2, 3 };
	// idVec4's constructor leaves it uninitialized, so "= {}" would zero nothing
	std::array<idVec4, 2 * SET_VECTORS> joints;		// one 3 x 4 joint per set: rows X, Y, Z
	joints.fill( vec4_zero );
	for ( size_t set = 0; set < SHIFT.size(); set++ ) {
		idVec4 * rows = &joints[set * SET_VECTORS];
		rows[0].Set( 1.0f, 0.0f, 0.0f, SHIFT[set] );
		rows[1].Set( 0.0f, 1.0f, 0.0f, 0.0f );
		rows[2].Set( 0.0f, 0.0f, 1.0f, 0.0f );
	}

	fixture.NewList();
	const auto create = [&]( bufferType_t type, std::span<const std::byte> data ) {
		return fixture.buffers.Create( type, bufferUsage_t::FRAME, static_cast<int>( data.size() ), data.data() );
	};
	const bufferHandle_t vertexBuffer = create( bufferType_t::VERTEX, std::as_bytes( std::span( verts ) ) );
	const bufferHandle_t indexBuffer = create( bufferType_t::INDEX, std::as_bytes( std::span( INDEXES ) ) );
	const bufferHandle_t jointBuffer = create( bufferType_t::JOINT, std::as_bytes( std::span( joints ) ) );
	if ( !Check( vertexBuffer != bufferHandle_t::NONE && indexBuffer != bufferHandle_t::NONE && jointBuffer != bufferHandle_t::NONE, "the FRAME set" ) ) {
		return;
	}
	Check( fixture.buffers.Get( jointBuffer ).placement == placement, va( "the joints in %s", D3D12_PlacementName( placement ) ) );

	std::array<idVec4, MAX_RENDER_PARMS> parms;
	parms.fill( vec4_zero );
	for ( int row = 0; row < 4; row++ ) {
		parms[RENDERPARM_MVPMATRIX_X + row][row] = 1.0f;	// the identity: the joints' positions are GL clip space
	}
	renderLatch_t latch;
	latch.stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
	latch.viewport = { 0, 0, SIZE, SIZE };
	latch.scissor = latch.viewport;
	latch.program = program;
	renderLatch_t clearLatch = latch;	// glClear obeys glDepthMask: this one writes depth
	clearLatch.stateBits = 0;
	clearDesc_t clear = {};
	clear.rect = latch.viewport;
	clear.color = clear.depth = clear.stencil = true;
	clear.colorWriteMask = 0xF;
	std::fill( std::begin( clear.rgba ), std::end( clear.rgba ), 1.0f );
	fixture.recorder.Clear( clearLatch, clear );

	pipelineKey_t key = {};
	key.stateBits = latch.stateBits & GLS_PIPELINE_BITS;
	key.layout = LAYOUT_DRAW_VERT;
	drawCall_t dc = {};
	dc.vertexBuffer = vertexBuffer;
	dc.indexBuffer = indexBuffer;
	dc.jointBuffer = jointBuffer;
	dc.numIndexes = static_cast<int>( INDEXES.size() );
	dc.numJoints = 1;
	dc.layout = LAYOUT_DRAW_VERT;
	dc.topology = primitive_t::TRIANGLES;
	dc.parms = parms.data();
	fixture.recorder.Draw( latch, key, dc );
	fixture.NextList();
	dc.jointOffset = SET_BYTES;
	fixture.recorder.Draw( latch, key, dc );
	const d3d12DrawStats_t stats = fixture.recorder.TakeStats();
	Check( stats.draws == 2 && stats.skipped == 0, va( "two draws recorded (%d, %d skipped)", stats.draws, stats.skipped ) );

	const std::vector<uint32> texels = fixture.ReadColor();
	if ( Check( texels.size() == static_cast<size_t>( SIZE ) * SIZE, "the scene is read" ) ) {
		int wrong = 0;
		for ( int row = 0; row < SIZE; row++ ) {
			for ( int column = 0; column < SIZE; column++ ) {
				const float x = ( static_cast<float>( column ) + 0.5f ) * 2.0f / SIZE - 1.0f;
				const float y = ( static_cast<float>( row ) + 0.5f ) * 2.0f / SIZE - 1.0f;
				const bool covered = std::ranges::any_of( SHIFT, [&]( float shift ) { return idMath::Fabs( x - shift ) < HALF_WIDTH; } )
					&& idMath::Fabs( y ) < HALF_HEIGHT;
				wrong += ( texels[static_cast<size_t>( row ) * SIZE + column] != ( covered ? 0xFF000000u : 0xFFFFFFFFu ) ) ? 1 : 0;
			}
		}
		Check( wrong == 0, va( "%s: %d pixels unlike both quads, black over white", D3D12_PlacementName( placement ), wrong ) );
	}
	fixture.buffers.Destroy( vertexBuffer );
	fixture.buffers.Destroy( indexBuffer );
	fixture.buffers.Destroy( jointBuffer );
	CheckNoLayerErrors( gpu );
}

// texture.vfp over three stripes: a red, a green and no texture (the black fallback, R11), the last in a new list.
// The views and samplers come out of the heaps by the b3 indices of each draw.
void DrawBindless( idTestGpu & gpu ) {
	constexpr int SIZE = idTestRecorder::SIZE;
	idTestRecorder fixture( gpu, d3d12BufferPlacement_t::UPLOAD );
	if ( !fixture.Ready() ) {
		return;
	}
	const programHandle_t program = fixture.Program( "texture" );
	const d3d12Program_t * linked = fixture.shaders.FindProgram( program );
	if ( !Check( linked != nullptr && linked->info.samplerMask == 1 && linked->info.cubeMask == 0, "texture samples slot 0, 2D" ) ) {
		return;
	}
	const auto texture = [&]( byte r, byte g, const char * name ) {
		const textureHandle_t handle = gpu.textures.Create( Opts( FMT_RGBA8, 1, 1, 1 ), textureRole_t::SAMPLED, POINT_CLAMP, name );
		const byte texel[4] = { r, g, 0, 255 };
		gpu.textures.Upload( handle, 0, 0, 1, 1, texel, 0 );
		return handle;
	};
	const textureHandle_t red = texture( 255, 0, "test red" );
	const textureHandle_t green = texture( 0, 255, "test green" );

	constexpr int STRIPES = 3;
	std::array<idDrawVert, 4 * STRIPES> verts;
	std::array<triIndex_t, 6 * STRIPES> indexes;
	for ( int stripe = 0; stripe < STRIPES; stripe++ ) {
		const float left = -1.0f + 2.0f * stripe / STRIPES;
		const float right = left + 2.0f / STRIPES;
		const float corners[4][2] = { { left, -1.0f }, { right, -1.0f }, { right, 1.0f }, { left, 1.0f } };
		for ( int i = 0; i < 4; i++ ) {
			idDrawVert & vert = verts[stripe * 4 + i];
			vert.Clear();
			vert.xyz.Set( corners[i][0], corners[i][1], 0.0f );
			vert.SetTexCoord( 0.5f, 0.5f );
		}
		constexpr triIndex_t QUAD[6] = { 0, 1, 2, 0, 2, 3 };
		for ( int i = 0; i < 6; i++ ) {
			indexes[stripe * 6 + i] = static_cast<triIndex_t>( stripe * 4 + QUAD[i] );
		}
	}
	fixture.NewList();
	const bufferHandle_t vertexBuffer = fixture.buffers.Create( bufferType_t::VERTEX, bufferUsage_t::FRAME, static_cast<int>( sizeof( verts ) ), verts.data() );
	const bufferHandle_t indexBuffer = fixture.buffers.Create( bufferType_t::INDEX, bufferUsage_t::FRAME, static_cast<int>( sizeof( indexes ) ), indexes.data() );
	if ( !Check( vertexBuffer != bufferHandle_t::NONE && indexBuffer != bufferHandle_t::NONE && red != textureHandle_t::NONE
			&& green != textureHandle_t::NONE, "the buffers and the textures" ) ) {
		return;
	}
	std::array<idVec4, MAX_RENDER_PARMS> parms;
	parms.fill( vec4_zero );
	for ( int row = 0; row < 4; row++ ) {
		parms[RENDERPARM_MVPMATRIX_X + row][row] = 1.0f;
	}
	parms[RENDERPARM_TEXTUREMATRIX_S].Set( 1.0f, 0.0f, 0.0f, 0.0f );
	parms[RENDERPARM_TEXTUREMATRIX_T].Set( 0.0f, 1.0f, 0.0f, 0.0f );
	parms[RENDERPARM_COLOR].Set( 1.0f, 1.0f, 1.0f, 1.0f );
	renderLatch_t latch;
	latch.stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
	latch.viewport = { 0, 0, SIZE, SIZE };
	latch.scissor = latch.viewport;
	latch.program = program;
	pipelineKey_t key = {};
	key.stateBits = latch.stateBits & GLS_PIPELINE_BITS;
	key.layout = LAYOUT_DRAW_VERT;
	drawCall_t dc = {};
	dc.vertexBuffer = vertexBuffer;
	dc.indexBuffer = indexBuffer;
	dc.numIndexes = 6;
	dc.layout = LAYOUT_DRAW_VERT;
	dc.topology = primitive_t::TRIANGLES;
	dc.parms = parms.data();
	const std::array<textureHandle_t, STRIPES> stripes = { red, green, textureHandle_t::NONE };
	for ( int stripe = 0; stripe < STRIPES; stripe++ ) {
		if ( stripe == STRIPES - 1 ) {
			fixture.NextList();
		}
		latch.textures[0][static_cast<size_t>( textureKind_t::TEX_2D )] = stripes[stripe];
		dc.indexOffset = stripe * 6 * static_cast<int>( sizeof( triIndex_t ) );
		fixture.recorder.Draw( latch, key, dc );
	}
	const d3d12DrawStats_t stats = fixture.recorder.TakeStats();
	Check( stats.draws == STRIPES && stats.resourceBinds == STRIPES && stats.unboundSlots == 1,
		va( "%d draws, %d resource binds, %d unbound", stats.draws, stats.resourceBinds, stats.unboundSlots ) );

	const std::vector<uint32> texels = fixture.ReadColor();
	if ( Check( texels.size() == static_cast<size_t>( SIZE ) * SIZE, "the scene is read" ) ) {
		constexpr std::array<uint32, STRIPES> EXPECTED = { 0xFF0000FFu, 0xFF00FF00u, 0xFF000000u };
		int wrong = 0;
		for ( int row = 0; row < SIZE; row++ ) {
			for ( int column = 0; column < SIZE; column++ ) {
				wrong += ( texels[static_cast<size_t>( row ) * SIZE + column] != EXPECTED[static_cast<size_t>( ( column + 0.5f ) * STRIPES / SIZE )] ) ? 1 : 0;
			}
		}
		Check( wrong == 0, va( "%d pixels unlike red, green, black", wrong ) );
	}
	fixture.buffers.Destroy( vertexBuffer );
	fixture.buffers.Destroy( indexBuffer );
	gpu.textures.Destroy( red );
	gpu.textures.Destroy( green );
	CheckNoLayerErrors( gpu );
}

// Every value of r_d3d12FrameBuffers the adapter has.
void DrawFrameBufferPlacements( idTestGpu & gpu ) {
	for ( const d3d12BufferPlacement_t placement : { d3d12BufferPlacement_t::UPLOAD, d3d12BufferPlacement_t::STAGED, d3d12BufferPlacement_t::GPU_UPLOAD } ) {
		if ( placement == d3d12BufferPlacement_t::GPU_UPLOAD && !gpu.device.Features().gpuUploadHeap ) {
			printf( "  %s skipped: the adapter has no GPU upload heap\n", D3D12_PlacementName( placement ) );
			continue;
		}
		DrawFrameBuffers( gpu, placement );
	}
}

// ---- DXC and shader model 6.6 ------------------------------------------------------------------------------

// One pixel per value; the pixel shader finds the values through the heap index alone (bindless, SM 6.6).
constexpr std::string_view BINDLESS_HLSL = R"(
cbuffer drawIndices : register( b0 ) {
	uint valuesIndex;
};
float4 VSMain( uint vertexId : SV_VertexID ) : SV_Position {
	float2 corner = float2( ( vertexId << 1 ) & 2, vertexId & 2 );
	return float4( corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0 );
}
float4 PSMain( float4 position : SV_Position ) : SV_Target {
	StructuredBuffer<float4> values = ResourceDescriptorHeap[valuesIndex];
	return values[uint( position.x )];
}
)";

// Without a device: DXC loads from the exe's folder, signs, passes defines and names the source in an error.
void Test_DxcCompile() {
	auto library = idD3D12Library::Load();
	if ( !Check( library.has_value(), library ? "" : va( "idD3D12Library::Load: %s", library.error().c_str() ) ) ) {
		return;
	}
	Check( library->dxcVersion >= ( ( uint64( 1 ) << 48 ) | ( uint64( 9 ) << 32 ) ), "dxcompiler.dll is DXC 1.9 or newer" );
	const auto good = D3D12_DxcCompile( *library, BINDLESS_HLSL, "bindless test", "PSMain", "ps_6_6" );
	if ( Check( good.has_value(), good ? "" : va( "compile: %s", good.error().c_str() ) ) ) {
		Check( D3D12_DxilSigned( good->code ), "dxil.dll signed the DXIL" );
		Check( good->warnings.IsEmpty(), va( "no warnings: %s", good->warnings.c_str() ) );
	}
	constexpr std::string_view DEFINED = "#ifndef ANSWER\n#error ANSWER is not defined\n#endif\nfloat4 PSMain() : SV_Target { return ANSWER; }\n";
	const char * const defines[] = { "ANSWER=42" };
	const auto defined = D3D12_DxcCompile( *library, DEFINED, "define test", "PSMain", "ps_6_6", defines );
	Check( defined.has_value(), defined ? "" : va( "a define reaches the source: %s", defined.error().c_str() ) );
	const auto broken = D3D12_DxcCompile( *library, "float4 PSMain() : SV_Target { return undefinedName; }", "broken test", "PSMain", "ps_6_6" );
	Check( !broken && broken.error().Find( "undefinedName" ) >= 0 && broken.error().Find( "broken test" ) >= 0,
		va( "the error names the source and the identifier: %s", broken ? "(compiled)" : broken.error().c_str() ) );
}

// Feature level 12_2 and SM 6.6: a root signature with the heap directly indexed, values read through ResourceDescriptorHeap.
void DxcBindless( idTestGpu & gpu ) {
	constexpr UINT POINTS = 16;			// a row of 16 float4 is the 256-byte pitch a texture copy needs
	constexpr UINT VALUES_SLOT = 5;		// not 0: the index must reach the shader
	ID3D12Device2 & device = gpu.device.Get();
	Check( gpu.device.Features().shaderModel >= D3D_SHADER_MODEL_6_6, "the device runs shader model 6.6" );
	printf( "  %s\n", gpu.device.Features().versionString.c_str() );

	const auto vertex = D3D12_DxcCompile( gpu.library, BINDLESS_HLSL, "bindless test", "VSMain", "vs_6_6" );
	const auto pixel = D3D12_DxcCompile( gpu.library, BINDLESS_HLSL, "bindless test", "PSMain", "ps_6_6" );
	if ( !Check( vertex && pixel, va( "compile the bindless test shaders: %s", !vertex ? vertex.error().c_str() : ( !pixel ? pixel.error().c_str() : "" ) ) ) ) {
		return;
	}

	D3D12_ROOT_PARAMETER1 parameter = {};
	parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameter.Constants = { 0, 0, 1 };
	parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
	desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = 1;
	desc.Desc_1_1.pParameters = &parameter;
	desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
	idComPtr<ID3DBlob> blob;
	idComPtr<ID3DBlob> error;
	idComPtr<ID3D12RootSignature> root;
	if ( !Check( SUCCEEDED( gpu.library.serializeRootSignature( &desc, &blob, &error ) ), "serialize the bindless root signature" )
		|| !gpu.device.Check( device.CreateRootSignature( 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS( &root ) ),
			"CreateRootSignature (bindless test)" ) ) {
		return;
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
	pso.pRootSignature = root.Get();
	pso.VS = { vertex->code.data(), vertex->code.size() };
	pso.PS = { pixel->code.data(), pixel->code.size() };
	pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pso.SampleMask = UINT_MAX;
	pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = 1;
	pso.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
	pso.SampleDesc.Count = 1;
	idComPtr<ID3D12PipelineState> pipeline;
	if ( !gpu.device.Check( device.CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( &pipeline ) ), "CreateGraphicsPipelineState (bindless test)" ) ) {
		return;
	}

	const D3D12_DESCRIPTOR_HEAP_DESC viewHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
	const D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
	idComPtr<ID3D12DescriptorHeap> viewHeap;
	idComPtr<ID3D12DescriptorHeap> rtvHeap;
	if ( !gpu.device.Check( device.CreateDescriptorHeap( &viewHeapDesc, IID_PPV_ARGS( &viewHeap ) ), "CreateDescriptorHeap (bindless views)" )
		|| !gpu.device.Check( device.CreateDescriptorHeap( &rtvHeapDesc, IID_PPV_ARGS( &rtvHeap ) ), "CreateDescriptorHeap (bindless target)" ) ) {
		return;
	}

	std::array<idVec4, POINTS> expected;
	for ( UINT i = 0; i < POINTS; i++ ) {
		expected[i] = idVec4( static_cast<float>( i ), 0.5f * static_cast<float>( i ), -static_cast<float>( i ), 1.0f );
	}
	const idComPtr<ID3D12Resource> values = D3D12_CreateBuffer( gpu.device, D3D12_HEAP_TYPE_UPLOAD, sizeof( expected ),
		D3D12_RESOURCE_STATE_GENERIC_READ, L"bindless test values" );
	const idComPtr<ID3D12Resource> readback = D3D12_CreateBuffer( gpu.device, D3D12_HEAP_TYPE_READBACK, sizeof( expected ),
		D3D12_RESOURCE_STATE_COPY_DEST, L"bindless test readback" );
	void * mapped = nullptr;
	const D3D12_RANGE nothingRead = { 0, 0 };
	if ( !values || !readback || !gpu.device.Check( values->Map( 0, &nothingRead, &mapped ), "Map (bindless test values)" ) ) {
		return;
	}
	memcpy( mapped, expected.data(), sizeof( expected ) );
	values->Unmap( 0, nullptr );
	D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
	view.Format = DXGI_FORMAT_UNKNOWN;
	view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	view.Buffer.NumElements = POINTS;
	view.Buffer.StructureByteStride = sizeof( idVec4 );
	D3D12_CPU_DESCRIPTOR_HANDLE slot = viewHeap->GetCPUDescriptorHandleForHeapStart();
	slot.ptr += static_cast<SIZE_T>( VALUES_SLOT ) * device.GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
	device.CreateShaderResourceView( values.Get(), &view, slot );

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC targetDesc = {};
	targetDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	targetDesc.Width = POINTS;
	targetDesc.Height = 1;
	targetDesc.DepthOrArraySize = 1;
	targetDesc.MipLevels = 1;
	targetDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	targetDesc.SampleDesc.Count = 1;
	targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	idComPtr<ID3D12Resource> target;
	if ( !gpu.device.Check( device.CreateCommittedResource( &defaultHeap, D3D12_HEAP_FLAG_NONE, &targetDesc, D3D12_RESOURCE_STATE_RENDER_TARGET,
			nullptr, IID_PPV_ARGS( &target ) ), "CreateCommittedResource (bindless test target)" ) ) {
		return;
	}
	d3d12ResourceState_t targetState;
	targetState.current = D3D12_RESOURCE_STATE_RENDER_TARGET;
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	device.CreateRenderTargetView( target.Get(), nullptr, rtv );

	ID3D12GraphicsCommandList1 & list = gpu.List();
	ID3D12DescriptorHeap * heaps[] = { viewHeap.Get() };
	list.SetDescriptorHeaps( 1, heaps );	// before the root signature: it indexes the heap set at that moment
	list.SetGraphicsRootSignature( root.Get() );
	list.SetPipelineState( pipeline.Get() );
	list.SetGraphicsRoot32BitConstant( 0, VALUES_SLOT, 0 );
	list.OMSetRenderTargets( 1, &rtv, FALSE, nullptr );
	const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>( POINTS ), 1.0f, 0.0f, 1.0f };
	const D3D12_RECT scissor = { 0, 0, static_cast<LONG>( POINTS ), 1 };
	list.RSSetViewports( 1, &viewport );
	list.RSSetScissorRects( 1, &scissor );
	list.IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	list.DrawInstanced( 3, 1, 0, 0 );
	gpu.barriers.Transition( *target.Get(), targetState, D3D12_RESOURCE_STATE_COPY_SOURCE );
	gpu.barriers.Flush();
	D3D12_TEXTURE_COPY_LOCATION destination = {};
	destination.pResource = readback.Get();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, POINTS, 1, 1, POINTS * sizeof( idVec4 ) };
	D3D12_TEXTURE_COPY_LOCATION source = {};
	source.pResource = target.Get();
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	list.CopyTextureRegion( &destination, 0, 0, 0, &source, nullptr );
	gpu.WaitIdle();

	std::array<idVec4, POINTS> drawn;
	const D3D12_RANGE everything = { 0, sizeof( drawn ) };
	void * read = nullptr;
	if ( gpu.device.Check( readback->Map( 0, &everything, &read ), "Map (bindless test readback)" ) ) {
		memcpy( drawn.data(), read, sizeof( drawn ) );
		const D3D12_RANGE nothingWritten = { 0, 0 };
		readback->Unmap( 0, &nothingWritten );
		for ( UINT i = 0; i < POINTS; i++ ) {
			Check( drawn[i] == expected[i], va( "pixel %u: %s, expected %s", i, drawn[i].ToString(), expected[i].ToString() ) );
		}
	}
	CheckNoLayerErrors( gpu );
}

// ---- WARP: the render graph --------------------------------------------------------------------------------

// FILL writes ( x, y, 1.5, 1 ), DOWN averages 2 x 2 texels of source into target, ADD adds 1; CLEAR and COUNT
// clear and add to word 0 of a raw buffer.
constexpr const char * GRAPH_COMPUTE =
	"cbuffer Parms : register( b0 ) { uint target; uint source; uint2 size; };\n"
	"cbuffer Dispatch : register( b1 ) { uint amount; };\n"
	"[numthreads( 8, 8, 1 )]\n"
	"void CSMain( uint3 id : SV_DispatchThreadID ) {\n"
	"#if defined( CLEAR ) || defined( COUNT )\n"
	"	RWByteAddressBuffer counter = ResourceDescriptorHeap[target];\n"
	"#if defined( CLEAR )\n"
	"	if ( all( id == 0 ) ) { counter.Store( 0, 0 ); }\n"
	"#else\n"
	"	counter.InterlockedAdd( 0, amount );\n"
	"#endif\n"
	"#else\n"
	"	RWTexture2D<float4> output = ResourceDescriptorHeap[target];\n"
	"	if ( any( id.xy >= size ) ) { return; }\n"
	"#if defined( FILL )\n"
	"	output[id.xy] = float4( id.x, id.y, 1.5, 1.0 );\n"
	"#elif defined( DOWN )\n"
	"	Texture2D<float4> input = ResourceDescriptorHeap[source];\n"
	"	const int2 at = int2( id.xy * 2 );\n"
	"	output[id.xy] = ( input.Load( int3( at, 0 ) ) + input.Load( int3( at + int2( 1, 0 ), 0 ) ) + input.Load( int3( at + int2( 0, 1 ), 0 ) )\n"
	"		+ input.Load( int3( at + int2( 1, 1 ), 0 ) ) ) * 0.25;\n"
	"#else\n"
	"	Texture2D<float4> input = ResourceDescriptorHeap[source];\n"
	"	output[id.xy] = input.Load( int3( id.xy, 0 ) ) + 1.0;\n"
	"#endif\n"
	"#endif\n"
	"}\n";

// The texel of source under the pixel, the counter's word 0 added to alpha.
constexpr const char * GRAPH_PIXEL =
	"cbuffer Parms : register( b1 ) { uint source; uint counter; };\n"
	"float4 PSMain( float4 position : SV_Position ) : SV_Target {\n"
	"	Texture2D<float4> input = ResourceDescriptorHeap[source];\n"
	"	ByteAddressBuffer count = ResourceDescriptorHeap[counter];\n"
	"	return input.Load( int3( position.xy, 0 ) ) + float4( 0.0, 0.0, 0.0, float( count.Load( 0 ) ) );\n"
	"}\n";

[[nodiscard]] d3d12HlslSource_t GraphSource( const char * text, const char * define ) {
	d3d12HlslSource_t source;
	source.name = va( "test_graph_%s", define );
	source.path = "shaders/test_graph.hlsl";
	source.text = text;
	source.defines.Append( va( "%s=1", define ) );
	return source;
}

[[nodiscard]] d3d12ComputePipeline_t GraphCompute( idTestCopies & fixture, const idD3D12ComputeRootSignature & root, const char * define ) {
	const auto built = fixture.hlslCompiler.Build( GraphSource( GRAPH_COMPUTE, define ), shaderStageType_t::COMPUTE, hlslParms_t::ANY );
	if ( !Check( built.has_value(), built ? "" : built.error().c_str() ) ) {
		return {};
	}
	return D3D12_CreateComputePipeline( fixture.gpu.device, root, built->code, built->reflection, L"test graph" );
}

// A committed resource the graph imports; its views are made by the caller.
[[nodiscard]] idComPtr<ID3D12Resource> GraphTarget( idTestGpu & gpu, const D3D12_RESOURCE_DESC & desc ) {
	const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	idComPtr<ID3D12Resource> resource;
	gpu.device.Check( gpu.device.Get().CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
		IID_PPV_ARGS( &resource ) ), "CreateCommittedResource (test graph target)" );
	return resource;
}

/*
	One graph of compute, raster and copy passes, built twice as two frames would build it:

	  fill      levels mip 0 = ( x, y, 1.5, 1 )                  16 x 16, 2 mips
	  down      levels mip 1 = the 2 x 2 average of mip 0         the same resource as SRV (mip 0) and UAV (mip 1)
	  unused    a transient nobody reads                          culled, never recorded
	  count     counter = 0, UAV barrier, += amount x 256         two dispatches in one pass
	  draw      output = levels mip 1 + ( 0, 0, 0, counter )      raster into an imported 8 x 8 target
	  copy      result = counter                                  buffer copy into an imported buffer
	  chain     a = fill, b = a + 1, c = b + 1, added = c + 1      c takes a's pooled resource
*/
void RenderGraph( idTestGpu & gpu ) {
	idTestCopies fixture( gpu, 1 );
	if ( !fixture.Ready() ) {
		return;
	}
	const idD3D12ComputeRootSignature computeRoot( gpu.device, gpu.library );
	idD3D12ResourcePool pool( gpu.device, gpu.graveyard, gpu.srvPool, gpu.rtvPool );
	idD3D12RenderGraph graph( gpu.device, gpu, gpu.barriers, pool, gpu.srvPool, gpu.rtvPool, fixture.constants, computeRoot );
	const d3d12ComputePipeline_t fill = GraphCompute( fixture, computeRoot, "FILL" );
	const d3d12ComputePipeline_t down = GraphCompute( fixture, computeRoot, "DOWN" );
	const d3d12ComputePipeline_t add = GraphCompute( fixture, computeRoot, "ADD" );
	const d3d12ComputePipeline_t clear = GraphCompute( fixture, computeRoot, "CLEAR" );
	const d3d12ComputePipeline_t count = GraphCompute( fixture, computeRoot, "COUNT" );
	const auto pixel = fixture.hlslCompiler.Build( GraphSource( GRAPH_PIXEL, "DRAW" ), shaderStageType_t::FRAGMENT, hlslParms_t::ANY );
	if ( !Check( pixel.has_value(), pixel ? "" : pixel.error().c_str() ) || !computeRoot.Get() || !fill || !down || !add || !clear || !count ) {
		return;
	}
	const idComPtr<ID3D12PipelineState> drawPso = fixture.passes.CreatePipeline( pixel->code, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
		static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ), L"test graph draw" );

	constexpr DXGI_FORMAT FLOAT4 = DXGI_FORMAT_R32G32B32A32_FLOAT;
	D3D12_RESOURCE_DESC textureDesc = {};
	textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	textureDesc.Width = 8;
	textureDesc.Height = 8;
	textureDesc.DepthOrArraySize = 1;
	textureDesc.MipLevels = 1;
	textureDesc.Format = FLOAT4;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	const idComPtr<ID3D12Resource> output = GraphTarget( gpu, textureDesc );
	textureDesc.Width = textureDesc.Height = 16;
	textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	const idComPtr<ID3D12Resource> added = GraphTarget( gpu, textureDesc );
	const idComPtr<ID3D12Resource> result = D3D12_CreateBuffer( gpu.device, D3D12_HEAP_TYPE_DEFAULT, 16, D3D12_RESOURCE_STATE_COMMON, L"test graph result" );
	if ( !Check( drawPso && output && added && result, "the graph's PSO and imported resources" ) ) {
		return;
	}
	d3d12ResourceState_t outputState, addedState, resultState;
	resultState.decays = true;
	const d3d12Descriptor_t outputRtv = gpu.rtvPool.Allocate();
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.Format = FLOAT4;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	gpu.device.Get().CreateRenderTargetView( output.Get(), &rtvDesc, gpu.rtvPool.Cpu( outputRtv ) );
	const d3d12Descriptor_t addedUav = gpu.srvPool.Allocate();
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = FLOAT4;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	gpu.srvPool.CreateUav( addedUav, *added.Get(), uavDesc );

	uint32 amount = 1;
	bool unusedRecorded = false;
	const auto build = [&]() {
		constexpr d3d12PoolDesc_t PLAIN = { .format = FLOAT4, .width = 16, .height = 16, .unorderedAccess = true };
		constexpr d3d12PoolDesc_t MIPPED = { .format = FLOAT4, .width = 16, .height = 16, .mips = 2, .unorderedAccess = true };
		constexpr d3d12PoolDesc_t COUNTER = { .kind = d3d12PoolDesc_t::kind_t::BUFFER, .width = 16, .unorderedAccess = true };
		using access = d3d12GraphAccess_t;
		using pass = d3d12GraphPass_t;
		const d3d12GraphResource_t levels = graph.Create( "test levels", MIPPED );
		const d3d12GraphResource_t counter = graph.Create( "test counter", COUNTER );
		const d3d12GraphResource_t unused = graph.Create( "test unused", PLAIN );
		const std::array<d3d12GraphResource_t, 3> chain = { graph.Create( "test chain a", PLAIN ), graph.Create( "test chain b", PLAIN ),
			graph.Create( "test chain c", PLAIN ) };
		const d3d12GraphResource_t outputs = graph.Import( "test output", { output.Get(), &outputState, {}, {}, gpu.rtvPool.Cpu( outputRtv ), 8, 8 } );
		const d3d12GraphResource_t results = graph.Import( "test result", { result.Get(), &resultState, {}, {}, {}, 16, 1 } );
		const d3d12GraphResource_t adds = graph.Import( "test added", { added.Get(), &addedState, {}, addedUav, {}, 16, 16 } );
		const auto dispatch = [&]( const d3d12ComputePipeline_t & pipeline, d3d12GraphResource_t target, std::optional<d3d12GraphResource_t> source,
			uint16 targetMip, uint16 sourceMip ) {
			return [&pipeline, target, source, targetMip, sourceMip]( idD3D12GraphContext & context ) {
				const uint32 size = context.Width( target, targetMip );
				const std::array<uint32, 4> parms = { context.Uav( target, targetMip ), source ? context.Srv( *source, sourceMip ) : 0, size, size };
				const d3d12UploadSpan_t span = context.Parms( parms );
				context.Dispatch( pipeline, &span, size, size );
			};
		};
		graph.AddPass( "fill", pass::COMPUTE, { { levels, access::UAV, 0 } }, dispatch( fill, levels, std::nullopt, 0, 0 ) );
		graph.AddPass( "down", pass::COMPUTE, { { levels, access::SRV, 0 }, { levels, access::UAV, 1 } }, dispatch( down, levels, levels, 1, 0 ) );
		graph.AddPass( "unused", pass::COMPUTE, { { unused, access::UAV, 0 } }, [&unusedRecorded]( idD3D12GraphContext & ) {
			unusedRecorded = true;
		} );
		graph.AddPass( "count", pass::COMPUTE, { { counter, access::UAV } }, [&, counter]( idD3D12GraphContext & context ) {
			const std::array<uint32, 4> parms = { context.Uav( counter ), 0, 0, 0 };
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Dispatch( clear, &span, 1 );
			context.UavBarrier( counter );
			context.Dispatch( count, &span, 16, 16, 1, std::span( &amount, 1 ) );
		} );
		graph.AddPass( "draw", pass::RASTER, { { levels, access::SRV, 1 }, { counter, access::SRV }, { outputs, access::RTV } },
			[&, levels, counter, outputs]( idD3D12GraphContext & context ) {
				const d3d12UploadSpan_t span = context.Parms( std::array<uint32, 2>{ context.Srv( levels, 1 ), context.Srv( counter ) } );
				context.Draw( *drawPso.Get(), outputs, 0, { 0, 0, 8, 8 }, span );
			} );
		graph.AddPass( "copy", pass::COPY, { { counter, access::COPY_SOURCE }, { results, access::COPY_DEST } },
			[counter, results]( idD3D12GraphContext & context ) {
				context.List().CopyBufferRegion( &context.Resource( results ), 0, &context.Resource( counter ), 0, 16 );
			} );
		graph.AddPass( "chain a", pass::COMPUTE, { { chain[0], access::UAV, 0 } }, dispatch( fill, chain[0], std::nullopt, 0, 0 ) );
		graph.AddPass( "chain b", pass::COMPUTE, { { chain[0], access::SRV }, { chain[1], access::UAV, 0 } }, dispatch( add, chain[1], chain[0], 0, 0 ) );
		graph.AddPass( "chain c", pass::COMPUTE, { { chain[1], access::SRV }, { chain[2], access::UAV, 0 } }, dispatch( add, chain[2], chain[1], 0, 0 ) );
		graph.AddPass( "chain out", pass::COMPUTE, { { chain[2], access::SRV }, { adds, access::UAV } }, dispatch( add, adds, chain[2], 0, 0 ) );
	};

	const auto checkFrame = [&]( const char * frame ) {
		const idD3D12RenderGraph::stats_t & stats = graph.LastStats();
		Check( stats.passes == 9 && stats.culled == 1 && stats.transients == 5, va( "%s: %d passes, %d culled, %d transients: not 9, 1, 5", frame,
			stats.passes, stats.culled, stats.transients ) );
		Check( !unusedRecorded, va( "%s: the culled pass was recorded", frame ) );
		const std::vector<idVec4> drawn = ReadTexels<idVec4>( gpu, *output.Get(), outputState );
		const std::vector<idVec4> chained = ReadTexels<idVec4>( gpu, *added.Get(), addedState );
		const std::vector<byte> counted = gpu.Read( *result.Get(), &resultState, 0, 4 );
		if ( !Check( drawn.size() == 64 && chained.size() == 256 && counted.size() == 4, va( "%s: the readbacks", frame ) ) ) {
			return;
		}
		uint32 total = 0;
		memcpy( &total, counted.data(), sizeof( total ) );
		Check( total == amount * 256, va( "%s: the counter is %u, not %u", frame, total, amount * 256 ) );
		int wrongDrawn = 0;
		int wrongChained = 0;
		for ( int y = 0; y < 16; y++ ) {
			for ( int x = 0; x < 16; x++ ) {
				const idVec4 chainExpected( x + 3.0f, y + 3.0f, 4.5f, 4.0f );
				wrongChained += ( chained[y * 16 + x] != chainExpected ) ? 1 : 0;
				if ( x < 8 && y < 8 ) {
					const idVec4 drawExpected( x * 2.0f + 0.5f, y * 2.0f + 0.5f, 1.5f, 1.0f + static_cast<float>( amount * 256 ) );
					wrongDrawn += ( drawn[y * 8 + x] != drawExpected ) ? 1 : 0;
				}
			}
		}
		Check( wrongDrawn == 0, va( "%s: %d of 64 drawn texels wrong, ( 0, 0 ) is %s", frame, wrongDrawn, drawn[0].ToString() ) );
		Check( wrongChained == 0, va( "%s: %d of 256 chained texels wrong, ( 0, 0 ) is %s", frame, wrongChained, chained[0].ToString() ) );
		CheckNoLayerErrors( gpu );
	};

	fixture.Bind();
	build();
	graph.Execute();
	checkFrame( "frame 1" );
	// levels, the counter and two of the three chain textures: c reuses a's
	Check( pool.CreatedThisFrame() == 4 && pool.Num() == 4, va( "frame 1: %d pooled resources, %d created: not 4", pool.Num(), pool.CreatedThisFrame() ) );
	pool.EndFrame();

	amount = 2;
	fixture.Bind();
	build();
	graph.Execute();
	checkFrame( "frame 2" );
	Check( pool.CreatedThisFrame() == 0 && pool.Num() == 4, va( "frame 2: %d pooled resources, %d created: not 4 and 0", pool.Num(), pool.CreatedThisFrame() ) );

	for ( uint64 frame = 0; frame < idD3D12ResourcePool::UNUSED_FRAMES; frame++ ) {
		pool.EndFrame();
	}
	Check( pool.Num() == 4, va( "%d pooled resources after %llu unused frames: not 4", pool.Num(), idD3D12ResourcePool::UNUSED_FRAMES ) );
	pool.EndFrame();
	Check( pool.Num() == 0 && pool.Bytes() == 0, va( "%d pooled resources after %llu unused frames: not 0", pool.Num(), idD3D12ResourcePool::UNUSED_FRAMES + 1 ) );
	gpu.WaitIdle();
	gpu.srvPool.Free( addedUav );
	gpu.rtvPool.Free( outputRtv );
	CheckNoLayerErrors( gpu );
}

// ---- WARP: the HDR scene (R13) -----------------------------------------------------------------------------

[[nodiscard]] float HalfToFloat( uint16 half ) {
	const int exponent = ( half >> 10 ) & 0x1F;
	const int mantissa = half & 0x3FF;
	float value = 0.0f;
	if ( exponent == 0 ) {
		value = std::ldexp( static_cast<float>( mantissa ), -24 );
	} else if ( exponent == 31 ) {
		value = ( mantissa != 0 ) ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
	} else {
		value = std::ldexp( static_cast<float>( mantissa | 0x400 ), exponent - 25 );
	}
	return ( ( half & 0x8000 ) != 0 ) ? -value : value;
}

// An RGBA16F texel as read back.
[[nodiscard]] idVec4 HalfTexel( uint64 texel ) {
	return idVec4( HalfToFloat( static_cast<uint16>( texel ) ), HalfToFloat( static_cast<uint16>( texel >> 16 ) ),
		HalfToFloat( static_cast<uint16>( texel >> 32 ) ), HalfToFloat( static_cast<uint16>( texel >> 48 ) ) );
}

[[nodiscard]] double SrgbEncodeRef( double c ) {
	return ( c <= 0.0031308 ) ? c * 12.92 : 1.055 * std::pow( c, 1.0 / 2.4 ) - 0.055;
}

[[nodiscard]] double SrgbDecodeRef( double c ) {
	return ( c <= 0.04045 ) ? c / 12.92 : std::pow( ( c + 0.055 ) / 1.055, 2.4 );
}

// include/srgb.hlsli against the CPU: 256 inputs, below and above 1, and the round trip.
constexpr const char * COLORSPACE_COMPUTE =
	"#include \"include/srgb.hlsli\"\n"
	"cbuffer Parms : register( b0 ) { uint target; };\n"
	"[numthreads( 64, 1, 1 )]\n"
	"void CSMain( uint3 id : SV_DispatchThreadID ) {\n"
	"	RWTexture2D<float4> output = ResourceDescriptorHeap[target];\n"
	"	const float v = id.x / 255.0;\n"
	"	output[id.xy] = float4( SRGBToLinear( v.xxx ).x, LinearToSRGB( v.xxx ).x, SRGBToLinear( ( v * 4.0 ).xxx ).x,\n"
	"		LinearToSRGB( SRGBToLinear( v.xxx ) ).x );\n"
	"}\n";

void ColorSpace( idTestGpu & gpu ) {
	idTestCopies fixture( gpu, 1 );
	if ( !fixture.Ready() ) {
		return;
	}
	const idD3D12ComputeRootSignature computeRoot( gpu.device, gpu.library );
	idD3D12ResourcePool pool( gpu.device, gpu.graveyard, gpu.srvPool, gpu.rtvPool );
	idD3D12RenderGraph graph( gpu.device, gpu, gpu.barriers, pool, gpu.srvPool, gpu.rtvPool, fixture.constants, computeRoot );
	d3d12HlslSource_t source;
	source.name = "test_colorspace";
	source.path = "shaders/test_colorspace.hlsl";
	source.text = COLORSPACE_COMPUTE;
	const auto built = fixture.hlslCompiler.Build( source, shaderStageType_t::COMPUTE, hlslParms_t::ANY );
	if ( !Check( built.has_value(), built ? "" : built.error().c_str() ) ) {
		return;
	}
	const d3d12ComputePipeline_t pipeline = D3D12_CreateComputePipeline( gpu.device, computeRoot, built->code, built->reflection, L"test colorspace" );
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = 256;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	const idComPtr<ID3D12Resource> target = GraphTarget( gpu, desc );
	if ( !Check( pipeline && target, "the pipeline and the target" ) ) {
		return;
	}
	d3d12ResourceState_t state;
	const d3d12Descriptor_t uav = gpu.srvPool.Allocate();
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = desc.Format;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	gpu.srvPool.CreateUav( uav, *target.Get(), uavDesc );

	fixture.Bind();
	const d3d12GraphResource_t output = graph.Import( "colorspace", { target.Get(), &state, {}, uav, {}, 256, 1 } );
	graph.AddPass( "colorspace", d3d12GraphPass_t::COMPUTE, { { output, d3d12GraphAccess_t::UAV, 0 } }, [&]( idD3D12GraphContext & context ) {
		const uint32 index = context.Uav( output );
		const d3d12UploadSpan_t parms = context.Parms( index );
		context.Dispatch( pipeline, &parms, 256 );
	} );
	graph.Execute();
	const std::vector<idVec4> texels = ReadTexels<idVec4>( gpu, *target.Get(), state );
	if ( Check( texels.size() == 256, "the curves are read" ) ) {
		double worst[4] = {};
		for ( int i = 0; i < 256; i++ ) {
			const double v = i / 255.0;
			const double expected[4] = { SrgbDecodeRef( v ), SrgbEncodeRef( v ), SrgbDecodeRef( v * 4.0 ), v };
			for ( int c = 0; c < 4; c++ ) {
				worst[c] = Max( worst[c], std::abs( texels[i][c] - expected[c] ) / Max( 1.0, expected[c] ) );
			}
		}
		Check( worst[0] < 2e-6 && worst[1] < 2e-6, va( "the curves within 2e-6 of the CPU's (%g, %g)", worst[0], worst[1] ) );
		Check( worst[2] < 4e-6, va( "above 1 the decode goes on with the same formula (%g)", worst[2] ) );
		Check( worst[3] < 4e-6, va( "decode then encode gives the input back (%g)", worst[3] ) );
	}
	gpu.srvPool.Free( uav );
	CheckNoLayerErrors( gpu );
}

/*
	Draws into the HDR scene with preExposure 0.5, four stripes of 16 columns, cleared to 0:

	  0  ONE ONE: colour 2 twice           radiance 4 → 2 in the scene, alpha max( 1, 1 ) = 1
	  1  ONE ONE: colour 1, then -1        the negative adds nothing: 0.5
	  2  ONE ONE: colour 1, alpha 0.75 x2  alpha by MAX: 0.75, where a UNORM target would clamp the sum at 1
	  3  ONE ZERO 1, then DST_COLOR ZERO   a factor of 0.5 multiplies the scene and is no radiance: 0.25
	  then stripe 0 copied into a float texture and drawn over stripe 3 by texture.vfp: SceneColor and
	  SceneOutput give back the 2 the scene held
*/
void HdrScene( idTestGpu & gpu ) {
	constexpr int SIZE = idTestRecorder::SIZE;
	constexpr int STRIPES = 4;
	constexpr int STRIPE = SIZE / STRIPES;
	constexpr float PRE_EXPOSURE = 0.5f;
	idTestRecorder fixture( gpu, d3d12BufferPlacement_t::UPLOAD );
	if ( !fixture.Ready() ) {
		return;
	}
	const programHandle_t color = fixture.Program( "color" );
	const programHandle_t texture = fixture.Program( "texture" );
	const d3d12Program_t * colorProgram = fixture.shaders.FindProgram( color );
	if ( !Check( colorProgram != nullptr && fixture.shaders.FindProgram( texture ) != nullptr, "color and texture" )
			|| !Check( colorProgram->colorConstants && colorProgram->info.samplerMask == 0, "color reads the colour constants of b3, no slot" ) ) {
		return;
	}
	std::array<idDrawVert, 4 * STRIPES> verts;
	std::array<triIndex_t, 6 * STRIPES> indexes;
	for ( int stripe = 0; stripe < STRIPES; stripe++ ) {
		const float left = -1.0f + 2.0f * stripe / STRIPES;
		const float right = left + 2.0f / STRIPES;
		const float corners[4][2] = { { left, -1.0f }, { right, -1.0f }, { right, 1.0f }, { left, 1.0f } };
		for ( int i = 0; i < 4; i++ ) {
			idDrawVert & vert = verts[stripe * 4 + i];
			vert.Clear();
			vert.xyz.Set( corners[i][0], corners[i][1], 0.0f );
			vert.SetTexCoord( 0.5f, 0.5f );
		}
		constexpr triIndex_t QUAD[6] = { 0, 1, 2, 0, 2, 3 };
		for ( int i = 0; i < 6; i++ ) {
			indexes[stripe * 6 + i] = static_cast<triIndex_t>( stripe * 4 + QUAD[i] );
		}
	}
	fixture.NewList();
	fixture.scene.EnsureHdr( gpu.barriers, gpu );
	if ( !Check( fixture.scene.Color( true ).resource != nullptr, "the HDR scene" ) ) {
		return;
	}
	const bufferHandle_t vertexBuffer = fixture.buffers.Create( bufferType_t::VERTEX, bufferUsage_t::FRAME, static_cast<int>( sizeof( verts ) ), verts.data() );
	const bufferHandle_t indexBuffer = fixture.buffers.Create( bufferType_t::INDEX, bufferUsage_t::FRAME, static_cast<int>( sizeof( indexes ) ), indexes.data() );
	std::array<idVec4, MAX_RENDER_PARMS> parms;
	parms.fill( vec4_zero );
	for ( int row = 0; row < 4; row++ ) {
		parms[RENDERPARM_MVPMATRIX_X + row][row] = 1.0f;
	}
	parms[RENDERPARM_TEXTUREMATRIX_S].Set( 1.0f, 0.0f, 0.0f, 0.0f );
	parms[RENDERPARM_TEXTUREMATRIX_T].Set( 0.0f, 1.0f, 0.0f, 0.0f );
	renderLatch_t latch;
	latch.viewport = { 0, 0, SIZE, SIZE };
	latch.scissor = latch.viewport;
	drawCall_t dc = {};
	dc.vertexBuffer = vertexBuffer;
	dc.indexBuffer = indexBuffer;
	dc.numIndexes = 6;
	dc.layout = LAYOUT_DRAW_VERT;
	dc.topology = primitive_t::TRIANGLES;
	dc.parms = parms.data();
	const auto draw = [&]( int stripe, programHandle_t program, uint64 blend, const idVec4 & rgba ) {
		latch.program = program;
		latch.stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK | blend;
		parms[RENDERPARM_COLOR] = rgba;
		pipelineKey_t key = {};
		key.stateBits = latch.stateBits & GLS_PIPELINE_BITS;
		key.layout = LAYOUT_DRAW_VERT;
		dc.indexOffset = stripe * 6 * static_cast<int>( sizeof( triIndex_t ) );
		fixture.recorder.Draw( latch, key, dc );
	};

	fixture.recorder.SetHdr( true, PRE_EXPOSURE );
	clearDesc_t clear = {};
	clear.rect = latch.viewport;
	clear.color = true;
	clear.colorWriteMask = 0xF;
	renderLatch_t clearLatch = latch;
	clearLatch.stateBits = 0;
	fixture.recorder.Clear( clearLatch, clear );
	constexpr uint64 ADD = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
	draw( 0, color, ADD, idVec4( 2.0f, 2.0f, 2.0f, 1.0f ) );
	draw( 0, color, ADD, idVec4( 2.0f, 2.0f, 2.0f, 1.0f ) );
	draw( 1, color, ADD, idVec4( 1.0f, 1.0f, 1.0f, 1.0f ) );
	draw( 1, color, ADD, idVec4( -1.0f, -1.0f, -1.0f, 1.0f ) );
	draw( 2, color, ADD, idVec4( 1.0f, 1.0f, 1.0f, 0.75f ) );
	draw( 2, color, ADD, idVec4( 1.0f, 1.0f, 1.0f, 0.75f ) );
	draw( 3, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO, idVec4( 1.0f, 1.0f, 1.0f, 1.0f ) );
	draw( 3, color, GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO, idVec4( 0.5f, 0.5f, 0.5f, 1.0f ) );

	const auto readScene = [&]() {
		fixture.recorder.EndPass();
		idD3D12SceneTarget::surface_t & hdr = fixture.scene.Color( true );
		const std::vector<uint64> raw = ReadTexels<uint64>( gpu, *hdr.resource.Get(), hdr.state );
		std::vector<idVec4> texels;
		for ( uint64 texel : raw ) {
			texels.push_back( HalfTexel( texel ) );
		}
		fixture.NewList();
		return texels;
	};
	const auto checkStripe = [&]( const std::vector<idVec4> & texels, int stripe, const idVec4 & expected, const char * what ) {
		int wrong = 0;
		for ( int row = 0; row < SIZE; row++ ) {
			for ( int column = stripe * STRIPE; column < ( stripe + 1 ) * STRIPE; column++ ) {
				wrong += texels[static_cast<size_t>( row ) * SIZE + column].Compare( expected, 1e-3f ) ? 0 : 1;
			}
		}
		const idVec4 & first = texels[static_cast<size_t>( stripe ) * STRIPE];
		Check( wrong == 0, va( "%s: %d pixels unlike %s (%s)", what, wrong, expected.ToString( 3 ), first.ToString( 3 ) ) );
	};
	std::vector<idVec4> texels = readScene();
	if ( Check( texels.size() == static_cast<size_t>( SIZE ) * SIZE, "the HDR scene is read" ) ) {
		checkStripe( texels, 0, idVec4( 2.0f, 2.0f, 2.0f, 1.0f ), "2 + 2 = 4 times 0.5" );
		checkStripe( texels, 1, idVec4( 0.5f, 0.5f, 0.5f, 1.0f ), "a negative output adds nothing" );
		checkStripe( texels, 2, idVec4( 1.0f, 1.0f, 1.0f, 0.75f ), "alpha by MAX" );
		checkStripe( texels, 3, idVec4( 0.25f, 0.25f, 0.25f, 1.0f ), "a factor is no radiance" );
	}

	const textureHandle_t copy = gpu.textures.Create( Opts( FMT_RGBA8, STRIPE, SIZE, 1 ), textureRole_t::COLOR_COPY, POINT_CLAMP, "test hdr copy" );
	idD3D12ScreenCopies copies( gpu, gpu.barriers, fixture.scene, gpu.textures, fixture.passes, gpu.rtvPool );
	fixture.recorder.EndPass();
	copies.Color( copy, 0, 0, STRIPE, SIZE, true );
	Check( gpu.textures.IsLinear( copy ), "a copy of the HDR scene is linear" );
	latch.textures[0][static_cast<size_t>( textureKind_t::TEX_2D )] = copy;
	draw( 3, texture, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO, idVec4( 1.0f, 1.0f, 1.0f, 1.0f ) );
	texels = readScene();
	if ( !texels.empty() ) {
		checkStripe( texels, 3, idVec4( 2.0f, 2.0f, 2.0f, 1.0f ), "copy, SceneColor, SceneOutput: the scene's value again" );
	}

	// the gamma composite, preExposure 0.5: the clear is enc( 0.5 * dec( c ) ), an alpha blend mixes in gamma,
	// a factor is not exposed
	fixture.recorder.SetHdrGamma( true );
	clear.rgba[0] = clear.rgba[1] = clear.rgba[2] = 0.5f;
	clear.rgba[3] = 1.0f;
	fixture.recorder.Clear( clearLatch, clear );
	draw( 0, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA, idVec4( 1.0f, 1.0f, 1.0f, 0.25f ) );
	draw( 1, color, GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO, idVec4( 0.5f, 0.5f, 0.5f, 1.0f ) );
	texels = readScene();
	const float cleared = R_LinearToSRGB( R_SRGBToLinear( 0.5f ) * PRE_EXPOSURE );
	const float white = R_LinearToSRGB( PRE_EXPOSURE );
	if ( !texels.empty() ) {
		const float mixed = 0.25f * white + 0.75f * cleared;
		checkStripe( texels, 0, idVec4( mixed, mixed, mixed, 0.8125f ), "an alpha blend in gamma" );
		checkStripe( texels, 1, idVec4( 0.5f * cleared, 0.5f * cleared, 0.5f * cleared, 1.0f ), "a factor in gamma is not exposed" );
		checkStripe( texels, 2, idVec4( cleared, cleared, cleared, 1.0f ), "a clear in gamma" );
	}
	fixture.recorder.SetHdr( false, PRE_EXPOSURE );
	Check( !fixture.recorder.HdrGamma(), "the end of the HDR scene ends the gamma composite" );
	fixture.recorder.EndPass();
	gpu.WaitIdle();
	fixture.buffers.Destroy( vertexBuffer );
	fixture.buffers.Destroy( indexBuffer );
	gpu.textures.Destroy( copy );
	CheckNoLayerErrors( gpu );
}

// A float copy of the HDR scene keeps its values; an LDR copy of the output into the same texture is UNORM as
// before; switching between the two formats swaps the spare in and creates nothing.
void HdrCopies( idTestGpu & gpu, UINT samples ) {
	constexpr int SIZE = idTestCopies::SIZE;
	idTestCopies fixture( gpu, samples );
	if ( !fixture.Ready() ) {
		return;
	}
	fixture.Bind();
	fixture.scene.EnsureHdr( gpu.barriers, gpu );
	if ( !Check( fixture.scene.Color( true ).resource != nullptr, "the HDR scene" ) ) {
		return;
	}
	const auto value = []( int column, int row ) {
		return idVec4( static_cast<float>( column / 4 ) * 0.25f, static_cast<float>( row / 4 ) * 0.5f, 3.0f, 0.5f );
	};
	for ( int row = 0; row < SIZE; row += 4 ) {
		for ( int column = 0; column < SIZE; column += 4 ) {
			const D3D12_RECT rect = { column, row, column + 4, row + 4 };
			const idVec4 rgba = value( column, row );
			gpu.List().ClearRenderTargetView( fixture.scene.Rtv( false, true ), rgba.ToFloatPtr(), 1, &rect );
		}
	}
	fixture.FillColor();
	const idTestCopies::box_t box = { 8, 4, 24, 20 };
	const textureHandle_t texture = gpu.textures.Create( Opts( FMT_RGBA8, box.width, box.height, 1 ), textureRole_t::COLOR_COPY, POINT_CLAMP,
		"test hdr copies" );
	d3d12Texture_t & made = gpu.textures.Get( texture );
	gpu.textures.ResetFrameCounters();

	fixture.copies.Color( texture, box.x, box.y, box.width, box.height, true );
	Check( made.surface.linear && made.surface.resource->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
		"a copy of the HDR scene: an RGBA16F surface, marked linear" );
	const std::vector<uint64> raw = ReadTexels<uint64>( gpu, *made.surface.resource.Get(), made.surface.state );
	int wrong = 0;
	for ( int row = 0; row < box.height && !raw.empty(); row++ ) {
		for ( int column = 0; column < box.width; column++ ) {
			wrong += HalfTexel( raw[static_cast<size_t>( row ) * box.width + column] ).Compare( value( box.x + column, box.y + row ) ) ? 0 : 1;
		}
	}
	Check( !raw.empty() && wrong == 0, va( "%u sample(s): %d texels unlike the HDR scene", samples, wrong ) );

	fixture.Bind();
	fixture.copies.Color( texture, box.x, box.y, box.width, box.height, false );
	Check( !made.surface.linear && made.surface.resource->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM, "an LDR copy: the UNORM surface" );
	const std::vector<uint32> ldr = fixture.Read( *made.surface.resource.Get(), made.surface.state );
	wrong = 0;
	for ( int row = 0; row < box.height && !ldr.empty(); row++ ) {
		for ( int column = 0; column < box.width; column++ ) {
			wrong += ( ldr[static_cast<size_t>( row ) * box.width + column] != fixture.ColorAt( box.x + column, box.y + row ) ) ? 1 : 0;
		}
	}
	Check( !ldr.empty() && wrong == 0, va( "%u sample(s): %d texels unlike the output", samples, wrong ) );
	for ( int i = 0; i < 3; i++ ) {
		fixture.Bind();
		fixture.copies.Color( texture, box.x, box.y, box.width, box.height, ( i & 1 ) == 0 );
	}
	Check( gpu.textures.CopySurfacesThisFrame() == 1, va( "HDR, LDR and back create one surface (%d)", gpu.textures.CopySurfacesThisFrame() ) );
	gpu.WaitIdle();
	gpu.textures.Destroy( texture );
	CheckNoLayerErrors( gpu );
}

void HdrCopiesAllSamples( idTestGpu & gpu ) {
	HdrCopies( gpu, 1 );
	HdrCopies( gpu, 4 );
}

// The CPU's reference of internal/combineLut.hlsl, in double.
[[nodiscard]] idVec3 AgxRef( const idVec3 & input, bool punchy ) {
	constexpr double INSET[3][3] = { { 0.842479062253094, 0.0423282422610123, 0.0423756549057051 },
		{ 0.0784335999999992, 0.878468636469772, 0.0784336 }, { 0.0792237451477643, 0.0791661274605434, 0.879142973793104 } };
	constexpr double OUTSET[3][3] = { { 1.19687900512017, -0.0528968517574562, -0.0529716355144438 },
		{ -0.0980208811401368, 1.15190312990417, -0.0980434501171241 }, { -0.0990297440797205, -0.0989611768448433, 1.15107367264116 } };
	constexpr double MIN_EV = -12.47393;
	constexpr double MAX_EV = 4.026069;
	double c[3];
	for ( int j = 0; j < 3; j++ ) {
		c[j] = 0.0;
		for ( int i = 0; i < 3; i++ ) {
			c[j] += Max( static_cast<double>( input[i] ), 1e-10 ) * INSET[i][j];
		}
		const double x = ( std::clamp( std::log2( c[j] ), MIN_EV, MAX_EV ) - MIN_EV ) / ( MAX_EV - MIN_EV );
		const double x2 = x * x;
		const double x4 = x2 * x2;
		c[j] = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
	}
	if ( punchy ) {
		const double luma = c[0] * 0.2126 + c[1] * 0.7152 + c[2] * 0.0722;
		for ( double & v : c ) {
			v = luma + 1.4 * ( std::pow( Max( v, 0.0 ), 1.35 ) - luma );
		}
	}
	idVec3 out;
	for ( int j = 0; j < 3; j++ ) {
		out[j] = static_cast<float>( c[0] * OUTSET[0][j] + c[1] * OUTSET[1][j] + c[2] * OUTSET[2][j] );
	}
	return out;
}

[[nodiscard]] idVec3 FilmicRef( const idVec3 & input ) {
	constexpr double TO_AP1[3][3] = { { 0.613097, 0.339523, 0.047379 }, { 0.070194, 0.916354, 0.013452 }, { 0.020616, 0.109570, 0.869815 } };
	constexpr double FROM_AP1[3][3] = { { 1.704859, -0.621715, -0.083299 }, { -0.130078, 1.140734, -0.010560 }, { -0.023964, -0.128975, 1.153013 } };
	constexpr double LUMA[3] = { 0.2722287168, 0.6740817658, 0.0536895174 };
	constexpr double SLOPE = 0.88, TOE = 0.55, SHOULDER = 0.26, BLACK = 0.0, WHITE = 0.04;
	double c[3];
	for ( int i = 0; i < 3; i++ ) {
		c[i] = Max( 0.0, TO_AP1[i][0] * input[0] + TO_AP1[i][1] * input[1] + TO_AP1[i][2] * input[2] );
	}
	double luma = c[0] * LUMA[0] + c[1] * LUMA[1] + c[2] * LUMA[2];
	for ( double & v : c ) {
		v = luma + 0.96 * ( v - luma );
	}
	const double toeScale = 1.0 + BLACK - TOE;
	const double shoulderScale = 1.0 + WHITE - SHOULDER;
	const double bt = ( 0.18 + BLACK ) / toeScale - 1.0;
	const double toeMatch = std::log10( 0.18 ) - 0.5 * std::log( ( 1.0 + bt ) / ( 1.0 - bt ) ) * ( toeScale / SLOPE );
	const double straightMatch = ( 1.0 - TOE ) / SLOPE - toeMatch;
	const double shoulderMatch = SHOULDER / SLOPE - straightMatch;
	for ( double & v : c ) {
		const double logColor = std::log10( Max( v, 1e-10 ) );
		const double straight = SLOPE * ( logColor + straightMatch );
		double toe = -BLACK + ( 2.0 * toeScale ) / ( 1.0 + std::exp( ( -2.0 * SLOPE / toeScale ) * ( logColor - toeMatch ) ) );
		double shoulder = ( 1.0 + WHITE ) - ( 2.0 * shoulderScale ) / ( 1.0 + std::exp( ( 2.0 * SLOPE / shoulderScale ) * ( logColor - shoulderMatch ) ) );
		toe = ( logColor < toeMatch ) ? toe : straight;
		shoulder = ( logColor > shoulderMatch ) ? shoulder : straight;
		double t = std::clamp( ( logColor - toeMatch ) / ( shoulderMatch - toeMatch ), 0.0, 1.0 );
		t = ( shoulderMatch < toeMatch ) ? 1.0 - t : t;
		t = ( 3.0 - 2.0 * t ) * t * t;
		v = toe + ( shoulder - toe ) * t;
	}
	luma = c[0] * LUMA[0] + c[1] * LUMA[1] + c[2] * LUMA[2];
	for ( double & v : c ) {
		v = Max( 0.0, luma + 0.93 * ( v - luma ) );
	}
	idVec3 out;
	for ( int i = 0; i < 3; i++ ) {
		out[i] = static_cast<float>( Max( 0.0, FROM_AP1[i][0] * c[0] + FROM_AP1[i][1] * c[1] + FROM_AP1[i][2] * c[2] ) );
	}
	return out;
}

[[nodiscard]] int ToByte( double encoded ) {
	return static_cast<int>( std::floor( std::clamp( encoded, 0.0, 1.0 ) * 255.0 + 0.5 ) );
}

/*
	The HDR resolve on a test scene of 8 x 8 blocks, each one colour, through the Combine LUT and the tonemap
	into the output, with and without MSAA:

	  clamp, neutral    the sRGB encoding of the clamped colour within 1 of 255, also between LUT texels
	  AgX, filmic       the CPU's curves within 1 of 255 for colours on LUT texels
	  the viewport      the pixels outside it keep the output's clear
	  the LUT           built once for the same parms, again when they change
*/
void CombineLut( idTestGpu & gpu, UINT samples ) {
	constexpr int SIZE = idTestCopies::SIZE;
	constexpr int BLOCK = 8;
	constexpr int BLOCKS = SIZE / BLOCK;
	idTestCopies fixture( gpu, samples );
	if ( !fixture.Ready() ) {
		return;
	}
	const idD3D12ComputeRootSignature computeRoot( gpu.device, gpu.library );
	idD3D12ResourcePool pool( gpu.device, gpu.graveyard, gpu.srvPool, gpu.rtvPool );
	idD3D12RenderGraph graph( gpu.device, gpu, gpu.barriers, pool, gpu.srvPool, gpu.rtvPool, fixture.constants, computeRoot );
	idD3D12HdrPipeline hdr( gpu.device, gpu, computeRoot, fixture.passes, gpu.samplers, gpu.srvPool );
	hdr.CreateDefaults();
	fixture.Bind();
	fixture.scene.EnsureHdr( gpu.barriers, gpu );
	if ( !Check( computeRoot.Get() && fixture.scene.Color( true ).resource, "the compute root signature and the HDR scene" ) ) {
		return;
	}
	const auto onTexel = []( int i ) { return d3d12Lut::Value( static_cast<float>( i ) / 31.0f ); };
	// grey and coloured; the first half lies on LUT texels in every channel
	std::array<idVec3, BLOCKS * BLOCKS> colors;
	for ( int i = 0; i < BLOCKS * BLOCKS; i++ ) {
		if ( i < 32 ) {
			colors[i] = ( i < 16 ) ? idVec3( onTexel( i * 2 ), onTexel( i * 2 ), onTexel( i * 2 ) )
				: idVec3( onTexel( ( i * 7 ) % 32 ), onTexel( ( i * 13 ) % 32 ), onTexel( ( i * 3 ) % 32 ) );
		} else {
			const float v = 0.0005f * std::pow( 1.25f, static_cast<float>( i - 32 ) );	// 0.0005 to 600, between texels
			colors[i] = idVec3( v, v * 0.6f, v * 1.3f );
		}
	}
	const auto colorAt = [&]( int column, int row ) -> const idVec3 & { return colors[( row / BLOCK ) * BLOCKS + column / BLOCK]; };
	idD3D12SceneTarget::surface_t & output = fixture.scene.Color();
	const auto fill = [&]() {
		fixture.Bind();
		idD3D12SceneTarget::surface_t & scene = fixture.scene.Color( true );
		gpu.barriers.Transition( *scene.resource.Get(), scene.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
		gpu.barriers.Flush();
		for ( int row = 0; row < SIZE; row += BLOCK ) {
			for ( int column = 0; column < SIZE; column += BLOCK ) {
				const D3D12_RECT rect = { column, row, column + BLOCK, row + BLOCK };
				const idVec3 & c = colorAt( column, row );
				const float rgba[4] = { c.x, c.y, c.z, 1.0f };
				gpu.List().ClearRenderTargetView( fixture.scene.Rtv( false, true ), rgba, 1, &rect );
			}
		}
		gpu.barriers.Transition( *output.resource.Get(), output.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
		gpu.barriers.Flush();
		const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
		gpu.List().ClearRenderTargetView( fixture.scene.Rtv( false ), grey, 0, nullptr );
	};
	const auto run = [&]( const postProcessSettings_t & settings, const renderRect_t & viewport ) {
		fill();
		hdr.Resolve( graph, fixture.scene, settings, viewport, 1.0f, hdrExposureStep_t(), nullptr );
		idD3D12SceneTarget::surface_t & resolved = fixture.scene.Resolved( gpu.barriers, gpu );
		return fixture.Read( *resolved.resource.Get(), resolved.state );
	};
	const auto compare = [&]( const std::vector<uint32> & texels, const char * what, int blocks, auto expected ) {
		int worst = 0;
		int worstBlock = -1;
		for ( int row = 0; row < SIZE && !texels.empty(); row++ ) {
			for ( int column = 0; column < SIZE; column++ ) {
				const int block = ( row / BLOCK ) * BLOCKS + column / BLOCK;
				if ( block >= blocks ) {
					continue;
				}
				const idVec3 want = expected( colorAt( column, row ) );
				const uint32 texel = texels[static_cast<size_t>( row ) * SIZE + column];
				for ( int c = 0; c < 3; c++ ) {
					const int difference = std::abs( static_cast<int>( ( texel >> ( c * 8 ) ) & 0xFF ) - ToByte( want[c] ) );
					if ( difference > worst ) {
						worst = difference;
						worstBlock = block;
					}
				}
			}
		}
		Check( !texels.empty() && worst <= 1, va( "%u sample(s), %s: %d of 255 from the CPU's (block %d)", samples, what, worst, worstBlock ) );
	};
	const renderRect_t whole = { 0, 0, SIZE, SIZE };
	postProcessSettings_t settings;
	settings.autoExposure = false;
	settings.bloomQuality = 0;
	settings.dither = false;
	settings.tonemapper = hdrTonemapper_t::CLAMP;
	compare( run( settings, whole ), "clamp", BLOCKS * BLOCKS, []( const idVec3 & c ) {
		return idVec3( static_cast<float>( SrgbEncodeRef( std::clamp( c.x, 0.0f, 1.0f ) ) ), static_cast<float>( SrgbEncodeRef( std::clamp( c.y, 0.0f, 1.0f ) ) ),
			static_cast<float>( SrgbEncodeRef( std::clamp( c.z, 0.0f, 1.0f ) ) ) );
	} );
	static_cast<void>( run( settings, whole ) );
	Check( hdr.LutBuilds() == 1, va( "the same parms build the LUT once (%d)", hdr.LutBuilds() ) );
	settings.tonemapper = hdrTonemapper_t::AGX;
	compare( run( settings, whole ), "AgX", 32, []( const idVec3 & c ) { return AgxRef( c, false ); } );
	settings.agxLook = 1;
	compare( run( settings, whole ), "AgX punchy", 32, []( const idVec3 & c ) { return AgxRef( c, true ); } );
	settings.agxLook = 0;
	settings.tonemapper = hdrTonemapper_t::FILMIC;
	compare( run( settings, whole ), "filmic", 32, []( const idVec3 & c ) {
		const idVec3 toned = FilmicRef( c );
		return idVec3( static_cast<float>( SrgbEncodeRef( std::clamp( toned.x, 0.0f, 1.0f ) ) ), static_cast<float>( SrgbEncodeRef( std::clamp( toned.y, 0.0f, 1.0f ) ) ),
			static_cast<float>( SrgbEncodeRef( std::clamp( toned.z, 0.0f, 1.0f ) ) ) );
	} );
	Check( hdr.LutBuilds() == 4, va( "each new curve builds the LUT again (%d)", hdr.LutBuilds() ) );

	settings.tonemapper = hdrTonemapper_t::CLAMP;
	const renderRect_t inner = { 16, 8, 24, 40 };
	const std::vector<uint32> part = run( settings, inner );
	int outside = 0;
	for ( int row = 0; row < SIZE && !part.empty(); row++ ) {
		for ( int column = 0; column < SIZE; column++ ) {
			const bool inside = column >= inner.x && column < inner.x + inner.width && row >= inner.y && row < inner.y + inner.height;
			outside += ( !inside && part[static_cast<size_t>( row ) * SIZE + column] != 0xFF808080u ) ? 1 : 0;
		}
	}
	Check( !part.empty() && outside == 0, va( "%d pixels outside the viewport were written", outside ) );

	settings.dither = true;
	const std::vector<uint32> dithered = run( settings, whole );
	const std::vector<uint32> again = run( settings, whole );
	settings.dither = false;
	const std::vector<uint32> plain = run( settings, whole );
	int worst = 0;
	for ( size_t i = 0; i < plain.size() && i < dithered.size(); i++ ) {
		for ( int c = 0; c < 3; c++ ) {
			worst = Max( worst, std::abs( static_cast<int>( ( dithered[i] >> ( c * 8 ) ) & 0xFF ) - static_cast<int>( ( plain[i] >> ( c * 8 ) ) & 0xFF ) ) );
		}
	}
	Check( !dithered.empty() && dithered == again && worst <= 1, va( "the dither repeats and moves a byte by at most 1 (%d)", worst ) );
	gpu.WaitIdle();
	CheckNoLayerErrors( gpu );
}

// The gamma composite's conversion: inside the rectangle the sRGB encoding (above 1 too), outside nothing, and
// back to the values the scene had, with and without MSAA.
void HdrEncode( idTestGpu & gpu, UINT samples ) {
	constexpr int SIZE = idTestCopies::SIZE;
	constexpr int BLOCK = 8;
	idTestCopies fixture( gpu, samples );
	if ( !fixture.Ready() ) {
		return;
	}
	const idD3D12ComputeRootSignature computeRoot( gpu.device, gpu.library );
	idD3D12ResourcePool pool( gpu.device, gpu.graveyard, gpu.srvPool, gpu.rtvPool );
	idD3D12RenderGraph graph( gpu.device, gpu, gpu.barriers, pool, gpu.srvPool, gpu.rtvPool, fixture.constants, computeRoot );
	idD3D12HdrPipeline hdr( gpu.device, gpu, computeRoot, fixture.passes, gpu.samplers, gpu.srvPool );
	hdr.CreateDefaults();
	fixture.Bind();
	fixture.scene.EnsureHdr( gpu.barriers, gpu );
	idD3D12SceneTarget::surface_t & scene = fixture.scene.Color( true );
	if ( !Check( scene.resource != nullptr, "the HDR scene" ) ) {
		return;
	}
	const auto value = []( int column, int row ) {
		const float bx = static_cast<float>( column / BLOCK );
		const float by = static_cast<float>( row / BLOCK );
		return idVec4( bx / 1024.0f, by * 0.25f, 0.25f + bx * by * 0.5f, 0.25f );	// exact in fp16: the toe, 0..1.75, to 24.75
	};
	gpu.barriers.Transition( *scene.resource.Get(), scene.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	gpu.barriers.Flush();
	for ( int row = 0; row < SIZE; row += BLOCK ) {
		for ( int column = 0; column < SIZE; column += BLOCK ) {
			const D3D12_RECT rect = { column, row, column + BLOCK, row + BLOCK };
			gpu.List().ClearRenderTargetView( fixture.scene.Rtv( false, true ), value( column, row ).ToFloatPtr(), 1, &rect );
		}
	}
	const renderRect_t inner = { 16, 8, 32, 40 };
	const auto read = [&]() {
		idD3D12SceneTarget::surface_t & resolved = fixture.scene.Resolved( gpu.barriers, gpu, std::nullopt, true );
		std::vector<idVec4> texels;
		for ( uint64 texel : ReadTexels<uint64>( gpu, *resolved.resource.Get(), resolved.state ) ) {
			texels.push_back( HalfTexel( texel ) );
		}
		fixture.Bind();
		return texels;
	};
	const auto check = [&]( const std::vector<idVec4> & texels, bool encoded, const char * what ) {
		float worst = 0.0f;
		int worstAt = -1;
		int outside = 0;
		for ( int row = 0; row < SIZE && !texels.empty(); row++ ) {
			for ( int column = 0; column < SIZE; column++ ) {
				const bool inside = column >= inner.x && column < inner.x + inner.width && row >= inner.y && row < inner.y + inner.height;
				idVec4 want = value( column, row );
				if ( inside && encoded ) {
					want.Set( R_LinearToSRGB( want.x ), R_LinearToSRGB( want.y ), R_LinearToSRGB( want.z ), want.w );
				}
				const idVec4 & got = texels[static_cast<size_t>( row ) * SIZE + column];
				for ( int c = 0; c < 4; c++ ) {
					// in the encoded space, where one fp16 step of the encoding is what the round trip may lose
					const float gotEncoded = ( c < 3 && !encoded ) ? R_LinearToSRGB( got[c] ) : got[c];
					const float wantEncoded = ( c < 3 && !encoded ) ? R_LinearToSRGB( want[c] ) : want[c];
					const float error = idMath::Fabs( gotEncoded - wantEncoded ) / Max( 1.0f, idMath::Fabs( wantEncoded ) );
					worstAt = ( error > worst ) ? row * SIZE * 4 + column * 4 + c : worstAt;
					worst = Max( worst, error );
					outside += ( !inside && error > 0.0f ) ? 1 : 0;
				}
			}
		}
		Check( !texels.empty() && worst < 2e-3f && outside == 0, va( "%u sample(s), %s: worst %g (row %d, column %d, channel %d), %d changed outside",
			samples, what, worst, worstAt / ( SIZE * 4 ), ( worstAt / 4 ) % SIZE, worstAt % 4, outside ) );
	};
	hdr.Encode( graph, fixture.scene, true, inner, nullptr );
	check( read(), true, "encoded" );
	hdr.Encode( graph, fixture.scene, false, inner, nullptr );
	check( read(), false, "decoded again" );
	gpu.WaitIdle();
	CheckNoLayerErrors( gpu );
}

void HdrEncodeAllSamples( idTestGpu & gpu ) {
	HdrEncode( gpu, 1 );
	HdrEncode( gpu, 4 );
}

// The mean EV100 of counts between low and high of their weight, as internal/exposureAdapt.hlsl takes it, for a
// histogram from EV100 -8 to 8.
[[nodiscard]] double ExposureMeanRef( const std::array<double, d3d12Exposure::BINS> & counts, double low, double high ) {
	double total = 0.0;
	for ( const double count : counts ) {
		total += count;
	}
	double below = 0.0;
	double sum = 0.0;
	double weight = 0.0;
	for ( uint32 bin = 0; bin < d3d12Exposure::BINS; bin++ ) {
		const double inside = Max( 0.0, Min( below + counts[bin], total * high ) - Max( below, total * low ) );
		sum += inside * ( -8.0 + ( bin + 0.5 ) * 0.25 );
		weight += inside;
		below += counts[bin];
	}
	return sum / weight;
}

/*
	The eye adaptation on a scene of 8 x 8 blocks, each a grey at the centre of a histogram bin (EV100 -8 to 8, a
	quarter EV a bin), drawn with preExposure 2, with and without MSAA:

	  histogram   the read-back counts are the CPU's: 32 per 2 x 2 pixels, black and darker than the range none,
	              brighter in the last bin, only inside the viewport
	  measured    the mean between the percents within 1e-3 EV of the CPU's; with a centre-weighted mask too
	  adapted     a reset is the target (measured, clamped), a step moves by 1 - 2^( -seconds * speed ) up and down
	  exposure    auto 2^( compensation + bias - adapted ) / 1.2, manual the CPU's; the tonemap shows radiance times it
	  visualize   1 and 2 change the output, without a layer message
*/
void Exposure( idTestGpu & gpu, UINT samples ) {
	constexpr int SIZE = idTestCopies::SIZE;
	constexpr int BLOCK = 8;
	constexpr int BLOCKS = SIZE / BLOCK;
	constexpr float PRE_EXPOSURE = 2.0f;
	idTestCopies fixture( gpu, samples );
	if ( !fixture.Ready() ) {
		return;
	}
	const idD3D12ComputeRootSignature computeRoot( gpu.device, gpu.library );
	idD3D12ResourcePool pool( gpu.device, gpu.graveyard, gpu.srvPool, gpu.rtvPool );
	idD3D12RenderGraph graph( gpu.device, gpu, gpu.barriers, pool, gpu.srvPool, gpu.rtvPool, fixture.constants, computeRoot );
	idD3D12HdrPipeline hdr( gpu.device, gpu, computeRoot, fixture.passes, gpu.samplers, gpu.srvPool );
	hdr.CreateDefaults();
	fixture.Bind();
	fixture.scene.EnsureHdr( gpu.barriers, gpu );
	if ( !Check( computeRoot.Get() && fixture.scene.Color( true ).resource, "the compute root signature and the HDR scene" ) ) {
		return;
	}
	// every bin once (37 is odd), then black, darker and brighter than the range
	std::array<float, BLOCKS * BLOCKS> radiance;
	for ( int i = 0; i < BLOCKS * BLOCKS; i++ ) {
		radiance[i] = std::exp2( -8.0f + ( static_cast<float>( ( i * 37 ) % 64 ) + 0.5f ) * 0.25f - 3.0f );
	}
	radiance[5] = 0.0f;
	radiance[17] = std::exp2( -9.0f - 3.0f );
	radiance[42] = std::exp2( 10.0f - 3.0f );
	const auto radianceAt = [&]( int column, int row ) { return radiance[( row / BLOCK ) * BLOCKS + column / BLOCK]; };
	idD3D12SceneTarget::surface_t & output = fixture.scene.Color();
	const auto fill = [&]() {
		fixture.Bind();
		idD3D12SceneTarget::surface_t & scene = fixture.scene.Color( true );
		gpu.barriers.Transition( *scene.resource.Get(), scene.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
		gpu.barriers.Flush();
		for ( int row = 0; row < SIZE; row += BLOCK ) {
			for ( int column = 0; column < SIZE; column += BLOCK ) {
				const D3D12_RECT rect = { column, row, column + BLOCK, row + BLOCK };
				const float c = radianceAt( column, row ) * PRE_EXPOSURE;
				const float rgba[4] = { c, c, c, 1.0f };
				gpu.List().ClearRenderTargetView( fixture.scene.Rtv( false, true ), rgba, 1, &rect );
			}
		}
		gpu.barriers.Transition( *output.resource.Get(), output.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
		gpu.barriers.Flush();
		const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
		gpu.List().ClearRenderTargetView( fixture.scene.Rtv( false ), grey, 0, nullptr );
	};
	const auto expectedCounts = [&]( const renderRect_t & rect, float mask ) {
		std::array<double, d3d12Exposure::BINS> counts = {};
		const int blocksX = ( rect.width + 1 ) / 2;
		const int blocksY = ( rect.height + 1 ) / 2;
		for ( int y = 0; y < blocksY; y++ ) {
			for ( int x = 0; x < blocksX; x++ ) {
				const float ev = std::log2( radianceAt( rect.x + x * 2, rect.y + y * 2 ) ) + 3.0f;
				if ( !( ev >= -8.0f ) ) {
					continue;
				}
				const float dx = ( static_cast<float>( x ) + 0.5f ) / static_cast<float>( blocksX ) * 2.0f - 1.0f;
				const float dy = ( static_cast<float>( y ) + 0.5f ) / static_cast<float>( blocksY ) * 2.0f - 1.0f;
				const float weight = 1.0f + ( std::exp( -( dx * dx + dy * dy ) ) - 1.0f ) * mask;
				counts[Min( static_cast<int>( ( ev + 8.0f ) * 4.0f ), 63 )] += static_cast<double>( static_cast<uint32>( weight * 32.0f + 0.5f ) );
			}
		}
		return counts;
	};
	const auto run = [&]( const postProcessSettings_t & settings, const renderRect_t & viewport, const hdrExposureStep_t & step ) {
		fill();
		hdr.Resolve( graph, fixture.scene, settings, viewport, PRE_EXPOSURE, step, nullptr );
		gpu.WaitIdle();
		hdr.Poll();
		return hdr.LastExposure().value_or( d3d12ExposureReadback_t() );
	};
	const auto readOutput = [&]() {
		idD3D12SceneTarget::surface_t & resolved = fixture.scene.Resolved( gpu.barriers, gpu );
		return fixture.Read( *resolved.resource.Get(), resolved.state );
	};
	const auto within = []( double value, double want, double tolerance ) { return std::abs( value - want ) <= tolerance * Max( 1.0, std::abs( want ) ); };
	const auto wrongBins = []( const d3d12ExposureReadback_t & read, const std::array<double, d3d12Exposure::BINS> & counts ) {
		int wrong = 0;
		for ( uint32 bin = 0; bin < d3d12Exposure::BINS; bin++ ) {
			wrong += ( static_cast<double>( read.histogram[bin] ) != counts[bin] ) ? 1 : 0;
		}
		return wrong;
	};

	const renderRect_t whole = { 0, 0, SIZE, SIZE };
	postProcessSettings_t base;
	base.bloomQuality = 0;
	base.dither = false;
	base.tonemapper = hdrTonemapper_t::CLAMP;
	base.meteringMask = 0.0f;
	base.autoExposureBias = 0.0f;
	base.minEV100 = -8.0f;
	base.maxEV100 = 8.0f;
	base.lowPercent = 0.0f;
	base.highPercent = 1.0f;
	const hdrExposureStep_t reset;
	postProcessSettings_t settings = base;
	d3d12ExposureReadback_t read = run( settings, whole, reset );
	Check( hdr.LastExposure().has_value() && read.exposure.preExposure == PRE_EXPOSURE, "the EyeAdaptation is read back with its preExposure" );
	const std::array<double, d3d12Exposure::BINS> counts = expectedCounts( whole, 0.0f );
	Check( wrongBins( read, counts ) == 0, va( "%u sample(s): %d bins differ from the CPU's (the last %u, want %.0f)", samples, wrongBins( read, counts ),
		read.histogram[63], counts[63] ) );
	const double mean = ExposureMeanRef( counts, 0.0, 1.0 );
	Check( within( read.exposure.measuredEV100, mean, 1e-3 ), va( "%u sample(s): measured %.4f EV100, the CPU's %.4f", samples, read.exposure.measuredEV100, mean ) );
	const float stepped = std::nearbyint( read.exposure.measuredEV100 * d3d12Exposure::TARGET_STEPS ) / d3d12Exposure::TARGET_STEPS;
	Check( read.exposure.targetEV100 == stepped && read.exposure.adaptedEV100 == stepped, va( "a reset adapts at once to the target in steps: %.5f, want %.5f",
		read.exposure.adaptedEV100, stepped ) );
	const float autoExposure = std::exp2( -read.exposure.adaptedEV100 ) / 1.2f;
	Check( within( read.exposure.exposure, autoExposure, 1e-4 ), va( "exposure %g, want %g", read.exposure.exposure, autoExposure ) );
	const std::vector<uint32> plain = readOutput();
	int worst = 0;
	for ( int row = 0; row < SIZE && !plain.empty(); row++ ) {
		for ( int column = 0; column < SIZE; column++ ) {
			const double shown = SrgbEncodeRef( std::clamp( static_cast<double>( radianceAt( column, row ) * read.exposure.exposure ), 0.0, 1.0 ) );
			worst = Max( worst, std::abs( static_cast<int>( plain[static_cast<size_t>( row ) * SIZE + column] & 0xFF ) - ToByte( shown ) ) );
		}
	}
	Check( !plain.empty() && worst <= 1, va( "%u sample(s): the tonemap shows radiance times the exposure within %d of 255", samples, worst ) );

	settings.lowPercent = 0.25f;
	settings.highPercent = 0.75f;
	read = run( settings, whole, reset );
	const double middle = ExposureMeanRef( counts, 0.25, 0.75 );
	Check( within( read.exposure.measuredEV100, middle, 1e-3 ), va( "between 25 and 75 %%: %.4f EV100, the CPU's %.4f", read.exposure.measuredEV100, middle ) );
	settings.maxEV100 = read.exposure.measuredEV100 - 1.0f;
	read = run( settings, whole, reset );
	Check( read.exposure.targetEV100 == settings.maxEV100 && read.exposure.adaptedEV100 == settings.maxEV100, "the target stays at most maxEV100" );

	const float start = read.exposure.adaptedEV100;
	settings.minEV100 = settings.maxEV100 = start + 2.0f;
	settings.speedUp = 3.0f;
	settings.speedDown = 1.0f;
	read = run( settings, whole, { 0.25f, false } );
	const float up = start + 2.0f * ( 1.0f - std::exp2( -0.25f * 3.0f ) );
	Check( within( read.exposure.adaptedEV100, up, 1e-5 ), va( "toward a brighter target: %.5f, want %.5f", read.exposure.adaptedEV100, up ) );
	settings.minEV100 = settings.maxEV100 = start - 2.0f;
	read = run( settings, whole, { 0.25f, false } );
	const float down = up + ( start - 2.0f - up ) * ( 1.0f - std::exp2( -0.25f ) );
	Check( within( read.exposure.adaptedEV100, down, 1e-5 ), va( "toward a darker target: %.5f, want %.5f", read.exposure.adaptedEV100, down ) );
	read = run( settings, whole, { 0.0f, false } );
	Check( read.exposure.adaptedEV100 == down, "no time, no step" );
	settings.exposureCompensation = 1.0f;
	settings.autoExposureBias = -0.5f;
	read = run( settings, whole, reset );
	const float biased = std::exp2( 0.5f - read.exposure.adaptedEV100 ) / 1.2f;
	Check( read.exposure.adaptedEV100 == start - 2.0f && within( read.exposure.exposure, biased, 1e-4 ), "the compensation and the bias add to the auto exposure" );
	settings.autoExposure = false;
	settings.exposureCompensation = 1.5f;
	read = run( settings, whole, reset );
	Check( read.exposure.exposure == R_ManualExposure( settings ), "manual: the CPU's exposure" );

	settings = base;
	settings.meteringMask = 1.0f;
	read = run( settings, whole, reset );
	const double masked = ExposureMeanRef( expectedCounts( whole, 1.0f ), 0.0, 1.0 );
	Check( within( read.exposure.measuredEV100, masked, 0.01 ), va( "a centre-weighted mask: %.4f EV100, the CPU's %.4f (%.4f without)", read.exposure.measuredEV100,
		masked, mean ) );
	settings.meteringMask = 0.0f;
	const renderRect_t inner = { 16, 8, 32, 40 };
	read = run( settings, inner, reset );
	Check( wrongBins( read, expectedCounts( inner, 0.0f ) ) == 0, va( "%u sample(s): only the viewport counts", samples ) );

	settings.visualize = 1;
	static_cast<void>( run( settings, whole, reset ) );
	const std::vector<uint32> overlay = readOutput();
	settings.visualize = 2;
	static_cast<void>( run( settings, whole, reset ) );
	const std::vector<uint32> falseColor = readOutput();
	Check( !overlay.empty() && overlay != plain && !falseColor.empty() && falseColor != plain, "visualize 1 and 2 draw" );
	gpu.WaitIdle();
	CheckNoLayerErrors( gpu );
}

void ExposureAllSamples( idTestGpu & gpu ) {
	Exposure( gpu, 1 );
	Exposure( gpu, 4 );
}

// The 13-tap downsample of internal/bloomDownsample.hlsl without the Karis weights: ( w + 1 ) / 2 x ( h + 1 ) / 2
// texels from the w x h rect at x0, y0 of source (stride texels a row), the taps clamped to the rect's texel centres.
[[nodiscard]] std::vector<double> BloomDownsampleRef( const std::vector<double> & source, int stride, int x0, int y0, int w, int h ) {
	const auto tap = [&]( double x, double y ) {
		x = std::clamp( x, x0 + 0.5, x0 + w - 0.5 ) - 0.5;
		y = std::clamp( y, y0 + 0.5, y0 + h - 0.5 ) - 0.5;
		const int i = static_cast<int>( std::floor( x ) );
		const int j = static_cast<int>( std::floor( y ) );
		const double a = x - i;
		const double b = y - j;
		const auto at = [&]( int u, int v ) { return source[static_cast<size_t>( Min( v, y0 + h - 1 ) ) * stride + Min( u, x0 + w - 1 )]; };
		return ( at( i, j ) * ( 1.0 - a ) + at( i + 1, j ) * a ) * ( 1.0 - b ) + ( at( i, j + 1 ) * ( 1.0 - a ) + at( i + 1, j + 1 ) * a ) * b;
	};
	const int width = ( w + 1 ) / 2;
	const int height = ( h + 1 ) / 2;
	std::vector<double> target( static_cast<size_t>( width ) * height );
	for ( int v = 0; v < height; v++ ) {
		for ( int u = 0; u < width; u++ ) {
			const double x = x0 + ( u + 0.5 ) * 2.0;
			const double y = y0 + ( v + 0.5 ) * 2.0;
			const double corners = tap( x - 2, y - 2 ) + tap( x + 2, y - 2 ) + tap( x - 2, y + 2 ) + tap( x + 2, y + 2 );
			const double edges = tap( x, y - 2 ) + tap( x - 2, y ) + tap( x + 2, y ) + tap( x, y + 2 );
			const double inner = tap( x - 1, y - 1 ) + tap( x + 1, y - 1 ) + tap( x - 1, y + 1 ) + tap( x + 1, y + 1 );
			target[static_cast<size_t>( v ) * width + u] = tap( x, y ) * 0.125 + corners * 0.03125 + edges * 0.0625 + inner * 0.125;
		}
	}
	return target;
}

/*
	The bloom on a 64 x 64 scene (levels 32 x 32 down to 1 x 1), with and without MSAA, through the clamp curve at
	exposure 4; the blocks are dim, so the first level's Karis weights stay within 0.5 % of even:

	  flat       a flat field stays as it is with the bloom at intensity 1
	  block      the bloom alone of a centred 16 x 16 block is mirror-symmetric and spreads beyond the block; with
	             three levels, which stay off the edges, it keeps the block's energy (dithered, decoded) within 2 %
	  tints      only the last level's tint: one flat value, the 1 x 1 level of BloomDownsampleRef (from 4 x 4 on
	             the taps clamped at the edges lose a centred field's mean); only the first: nearly all of it
	             within 8 pixels of the block
	  viewport   a bright scene outside the viewport does not reach the bloom inside it
	  off        quality 0 and intensity 0 give the same bytes as no bloom; visualize 3 without levels the frame
*/
void Bloom( idTestGpu & gpu, UINT samples ) {
	constexpr int SIZE = idTestCopies::SIZE;
	idTestCopies fixture( gpu, samples );
	if ( !fixture.Ready() ) {
		return;
	}
	const idD3D12ComputeRootSignature computeRoot( gpu.device, gpu.library );
	idD3D12ResourcePool pool( gpu.device, gpu.graveyard, gpu.srvPool, gpu.rtvPool );
	idD3D12RenderGraph graph( gpu.device, gpu, gpu.barriers, pool, gpu.srvPool, gpu.rtvPool, fixture.constants, computeRoot );
	idD3D12HdrPipeline hdr( gpu.device, gpu, computeRoot, fixture.passes, gpu.samplers, gpu.srvPool );
	hdr.CreateDefaults();
	fixture.Bind();
	fixture.scene.EnsureHdr( gpu.barriers, gpu );
	if ( !Check( computeRoot.Get() && fixture.scene.Color( true ).resource, "the compute root signature and the HDR scene" ) ) {
		return;
	}
	idD3D12SceneTarget::surface_t & output = fixture.scene.Color();
	struct block_t {
		D3D12_RECT rect;
		float radiance;
	};
	// the scene: background, then the blocks; the output grey
	const auto run = [&]( const postProcessSettings_t & settings, const renderRect_t & viewport, float background, std::initializer_list<block_t> blocks ) {
		fixture.Bind();
		idD3D12SceneTarget::surface_t & scene = fixture.scene.Color( true );
		gpu.barriers.Transition( *scene.resource.Get(), scene.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
		gpu.barriers.Flush();
		const float back[4] = { background, background, background, 1.0f };
		gpu.List().ClearRenderTargetView( fixture.scene.Rtv( false, true ), back, 0, nullptr );
		for ( const block_t & block : blocks ) {
			const float rgba[4] = { block.radiance, block.radiance, block.radiance, 1.0f };
			gpu.List().ClearRenderTargetView( fixture.scene.Rtv( false, true ), rgba, 1, &block.rect );
		}
		gpu.barriers.Transition( *output.resource.Get(), output.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
		gpu.barriers.Flush();
		const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
		gpu.List().ClearRenderTargetView( fixture.scene.Rtv( false ), grey, 0, nullptr );
		hdr.Resolve( graph, fixture.scene, settings, viewport, 1.0f, hdrExposureStep_t(), nullptr );
		gpu.WaitIdle();
		idD3D12SceneTarget::surface_t & resolved = fixture.scene.Resolved( gpu.barriers, gpu );
		return fixture.Read( *resolved.resource.Get(), resolved.state );
	};
	const auto channel = []( const std::vector<uint32> & texels, int x, int y, int c ) {
		return static_cast<int>( ( texels[static_cast<size_t>( y ) * SIZE + x] >> ( c * 8 ) ) & 0xFF );
	};
	const auto energy = [&]( const std::vector<uint32> & texels, int x0, int y0, int x1, int y1 ) {
		double sum = 0.0;
		for ( int y = y0; y < y1; y++ ) {
			for ( int x = x0; x < x1; x++ ) {
				sum += SrgbDecodeRef( channel( texels, x, y, 0 ) / 255.0 );
			}
		}
		return sum;
	};

	constexpr float RADIANCE = 0.05f;
	constexpr float EXPOSURE = 4.0f;
	const renderRect_t whole = { 0, 0, SIZE, SIZE };
	postProcessSettings_t base;
	base.autoExposure = false;
	base.exposureCompensation = 2.0f;
	base.tonemapper = hdrTonemapper_t::CLAMP;
	base.dither = false;
	base.bloomIntensity = 1.0f;
	Check( d3d12Bloom::Levels( base, SIZE, SIZE ) == 6 && d3d12Bloom::Levels( base, 3, 3 ) == 2 && d3d12Bloom::Levels( base, 2, 2 ) == 1,
		"six levels for 64 x 64, as many as the mips of a small viewport" );

	postProcessSettings_t none = base;
	none.bloomQuality = 0;
	const std::vector<uint32> flatPlain = run( none, whole, RADIANCE, {} );
	const std::vector<uint32> flatBloom = run( base, whole, RADIANCE, {} );
	int worst = 0;
	for ( size_t i = 0; i < flatPlain.size() && flatPlain.size() == flatBloom.size(); i++ ) {
		for ( int c = 0; c < 3; c++ ) {
			worst = Max( worst, std::abs( static_cast<int>( ( flatPlain[i] >> ( c * 8 ) ) & 0xFF ) - static_cast<int>( ( flatBloom[i] >> ( c * 8 ) ) & 0xFF ) ) );
		}
	}
	Check( !flatBloom.empty() && worst <= 2, va( "%u sample(s): a flat field under a full bloom moves by %d of 255", samples, worst ) );

	const D3D12_RECT square = { 24, 24, 40, 40 };
	postProcessSettings_t alone = base;
	alone.visualize = 3;
	const std::vector<uint32> block = run( alone, whole, 0.0f, { { square, RADIANCE } } );
	int asymmetry = 0;
	for ( int y = 0; y < SIZE && !block.empty(); y++ ) {
		for ( int x = 0; x < SIZE; x++ ) {
			asymmetry = Max( asymmetry, std::abs( channel( block, x, y, 0 ) - channel( block, SIZE - 1 - x, y, 0 ) ) );
			asymmetry = Max( asymmetry, std::abs( channel( block, x, y, 0 ) - channel( block, x, SIZE - 1 - y, 0 ) ) );
		}
	}
	Check( !block.empty() && asymmetry <= 1, va( "%u sample(s): the bloom of a centred block is symmetric within %d of 255", samples, asymmetry ) );
	Check( !block.empty() && channel( block, 0, 0, 0 ) > 0 && channel( block, 20, 32, 0 ) > channel( block, 10, 32, 0 ), "the bloom spreads beyond the block" );
	postProcessSettings_t dithered = alone;
	dithered.dither = true;
	dithered.bloomQuality = 2;
	const std::vector<uint32> spread = run( dithered, whole, 0.0f, { { square, RADIANCE } } );
	const double blockEnergy = 16.0 * 16.0 * RADIANCE * EXPOSURE;
	const double spreadEnergy = spread.empty() ? 0.0 : energy( spread, 0, 0, SIZE, SIZE );
	Check( std::abs( spreadEnergy / blockEnergy - 1.0 ) <= 0.02, va( "%u sample(s): the bloom keeps %.4f of the block's energy", samples, spreadEnergy / blockEnergy ) );

	postProcessSettings_t last = alone;
	last.bloomTints.fill( vec3_zero );
	last.bloomTints[5] = idVec3( 1.0f, 1.0f, 1.0f );
	const std::vector<uint32> flat = run( last, whole, 0.0f, { { square, RADIANCE } } );
	int lowest = 255;
	int highest = 0;
	for ( int y = 0; y < SIZE && !flat.empty(); y++ ) {
		for ( int x = 0; x < SIZE; x++ ) {
			lowest = Min( lowest, channel( flat, x, y, 0 ) );
			highest = Max( highest, channel( flat, x, y, 0 ) );
		}
	}
	std::vector<double> level( static_cast<size_t>( SIZE ) * SIZE, 0.0 );
	for ( int y = square.top; y < square.bottom; y++ ) {
		for ( int x = square.left; x < square.right; x++ ) {
			level[static_cast<size_t>( y ) * SIZE + x] = RADIANCE;
		}
	}
	for ( int size = SIZE; size > 1; size /= 2 ) {
		level = BloomDownsampleRef( level, size, 0, 0, size, size );
	}
	const int smallest = ToByte( SrgbEncodeRef( level[0] * EXPOSURE ) );
	Check( !flat.empty() && highest - lowest <= 0 && std::abs( highest - smallest ) <= 1, va( "only the last level: one value %d .. %d, the CPU's %d (the mean %.5f, "
		"the 1 x 1 level %.5f)", lowest, highest, smallest, blockEnergy / EXPOSURE / ( SIZE * SIZE ), level[0] ) );
	postProcessSettings_t first = alone;
	first.bloomTints.fill( vec3_zero );
	first.bloomTints[0] = idVec3( 1.0f, 1.0f, 1.0f );
	first.dither = true;
	const std::vector<uint32> narrow = run( first, whole, 0.0f, { { square, RADIANCE } } );
	const double close = narrow.empty() ? 0.0 : energy( narrow, 16, 16, 48, 48 ) / Max( energy( narrow, 0, 0, SIZE, SIZE ), 1e-9 );
	Check( close >= 0.97, va( "only the first level: %.4f of the energy within 8 pixels of the block", close ) );

	const renderRect_t inner = { 16, 16, 32, 32 };
	const std::vector<uint32> confined = run( alone, inner, 4.0f, { { { 16, 16, 48, 48 }, 0.0f } } );
	int leaked = 0;
	for ( int y = inner.y; y < inner.y + inner.height && !confined.empty(); y++ ) {
		for ( int x = inner.x; x < inner.x + inner.width; x++ ) {
			leaked = Max( leaked, channel( confined, x, y, 0 ) );
		}
	}
	Check( !confined.empty() && leaked == 0, va( "%u sample(s): the scene outside the viewport stays out of its bloom (%d)",
		samples, leaked ) );

	postProcessSettings_t zero = base;
	zero.bloomIntensity = 0.0f;
	postProcessSettings_t noLevels = alone;
	noLevels.bloomQuality = 0;
	const std::vector<uint32> plain = run( none, whole, 0.0f, { { square, RADIANCE } } );
	Check( !plain.empty() && run( zero, whole, 0.0f, { { square, RADIANCE } } ) == plain && run( noLevels, whole, 0.0f, { { square, RADIANCE } } ) == plain,
		"intensity 0, and visualize 3 without levels, give the frame without bloom" );
	gpu.WaitIdle();
	CheckNoLayerErrors( gpu );
}

void BloomAllSamples( idTestGpu & gpu ) {
	Bloom( gpu, 1 );
	Bloom( gpu, 4 );
}

/*
	The local exposure on a 64 x 64 scene (one tile), with and without MSAA, through the clamp curve at exposure 1:

	  step      a left half of 0.05 and a right half of 0.6, 7 bins apart, under the grid alone: each half keeps its
	            own base up to the edge, so shadows of contrast 1.5 and highlights of 0.5 give 0.18 ( L / 0.18 )^c
	  detail    2 x 2 squares of 0.1 and 0.4 under the blurred luminance alone: their log mean 0.2 is the base,
	            detail 0 makes the frame flat at it, detail 2 puts the squares at 0.05 and 0.8
	  viewport  a bright scene outside the viewport does not reach its grid or blurred luminance
	  off       all 1, or the local exposure off, give the bytes without it; visualize 4 draws only when it is on
*/
void LocalExposure( idTestGpu & gpu, UINT samples ) {
	constexpr int SIZE = idTestCopies::SIZE;
	idTestCopies fixture( gpu, samples );
	if ( !fixture.Ready() ) {
		return;
	}
	const idD3D12ComputeRootSignature computeRoot( gpu.device, gpu.library );
	idD3D12ResourcePool pool( gpu.device, gpu.graveyard, gpu.srvPool, gpu.rtvPool );
	idD3D12RenderGraph graph( gpu.device, gpu, gpu.barriers, pool, gpu.srvPool, gpu.rtvPool, fixture.constants, computeRoot );
	idD3D12HdrPipeline hdr( gpu.device, gpu, computeRoot, fixture.passes, gpu.samplers, gpu.srvPool );
	hdr.CreateDefaults();
	fixture.Bind();
	fixture.scene.EnsureHdr( gpu.barriers, gpu );
	if ( !Check( computeRoot.Get() && fixture.scene.Color( true ).resource, "the compute root signature and the HDR scene" ) ) {
		return;
	}
	idD3D12SceneTarget::surface_t & output = fixture.scene.Color();
	struct fill_t {
		std::vector<D3D12_RECT> rects;
		float radiance;
	};
	const auto run = [&]( const postProcessSettings_t & settings, const renderRect_t & viewport, float background, const std::vector<fill_t> & fills ) {
		fixture.Bind();
		idD3D12SceneTarget::surface_t & scene = fixture.scene.Color( true );
		gpu.barriers.Transition( *scene.resource.Get(), scene.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
		gpu.barriers.Flush();
		const float back[4] = { background, background, background, 1.0f };
		gpu.List().ClearRenderTargetView( fixture.scene.Rtv( false, true ), back, 0, nullptr );
		for ( const fill_t & fill : fills ) {
			const float rgba[4] = { fill.radiance, fill.radiance, fill.radiance, 1.0f };
			gpu.List().ClearRenderTargetView( fixture.scene.Rtv( false, true ), rgba, static_cast<UINT>( fill.rects.size() ), fill.rects.data() );
		}
		gpu.barriers.Transition( *output.resource.Get(), output.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
		gpu.barriers.Flush();
		const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
		gpu.List().ClearRenderTargetView( fixture.scene.Rtv( false ), grey, 0, nullptr );
		hdr.Resolve( graph, fixture.scene, settings, viewport, 1.0f, hdrExposureStep_t(), nullptr );
		gpu.WaitIdle();
		idD3D12SceneTarget::surface_t & resolved = fixture.scene.Resolved( gpu.barriers, gpu );
		return fixture.Read( *resolved.resource.Get(), resolved.state );
	};
	// the largest channel difference from expected( x, y ) over rect; 255 for an empty read
	const auto worst = []( const std::vector<uint32> & texels, const renderRect_t & rect, const auto & expected ) {
		int most = texels.empty() ? 255 : 0;
		for ( int y = rect.y; y < rect.y + rect.height && !texels.empty(); y++ ) {
			for ( int x = rect.x; x < rect.x + rect.width; x++ ) {
				const int want = expected( x, y );
				for ( int c = 0; c < 3; c++ ) {
					most = Max( most, std::abs( static_cast<int>( ( texels[static_cast<size_t>( y ) * SIZE + x] >> ( c * 8 ) ) & 0xFF ) - want ) );
				}
			}
		}
		return most;
	};
	const auto encoded = []( double luminance ) { return ToByte( SrgbEncodeRef( luminance ) ); };
	const auto contrast = []( double luminance, double c ) { return 0.18 * std::pow( luminance / 0.18, c ); };

	const renderRect_t whole = { 0, 0, SIZE, SIZE };
	postProcessSettings_t base;
	base.autoExposure = false;
	base.exposureCompensation = 0.0f;
	base.tonemapper = hdrTonemapper_t::CLAMP;
	base.dither = false;
	base.bloomQuality = 0;
	base.localBlurredBlend = 0.0f;

	postProcessSettings_t step = base;
	step.localShadowContrast = 1.5f;
	step.localHighlightContrast = 0.5f;
	const std::vector<fill_t> halves = { { { { 0, 0, SIZE / 2, SIZE } }, 0.05f } };
	const std::vector<uint32> stepped = run( step, whole, 0.6f, halves );
	const int shadow = encoded( contrast( 0.05, 1.5 ) );
	const int highlight = encoded( contrast( 0.6, 0.5 ) );
	const int stepError = worst( stepped, whole, [&]( int x, int ) { return ( x < SIZE / 2 ) ? shadow : highlight; } );
	Check( stepError <= 1, va( "%u sample(s): the grid keeps each half's base up to the edge (%d, %d), off by %d", samples, shadow, highlight, stepError ) );

	postProcessSettings_t flat = base;
	flat.localBlurredBlend = 1.0f;
	flat.localDetailStrength = 0.0f;
	fill_t squares = { {}, 0.4f };
	for ( int y = 0; y < SIZE; y += 2 ) {
		for ( int x = ( y / 2 & 1 ) * 2; x < SIZE; x += 4 ) {
			squares.rects.push_back( { x, y, x + 2, y + 2 } );
		}
	}
	const std::vector<uint32> flattened = run( flat, whole, 0.1f, { squares } );
	const int flatError = worst( flattened, whole, [&]( int, int ) { return encoded( 0.2 ); } );
	Check( flatError <= 1, va( "%u sample(s): detail 0 makes the squares the blurred base 0.2, off by %d", samples, flatError ) );
	postProcessSettings_t sharp = flat;
	sharp.localDetailStrength = 2.0f;
	const std::vector<uint32> sharpened = run( sharp, whole, 0.1f, { squares } );
	const int sharpError = worst( sharpened, whole, [&]( int x, int y ) { return encoded( ( ( x / 2 + y / 2 ) & 1 ) ? 0.05 : 0.8 ); } );
	Check( sharpError <= 1, va( "%u sample(s): detail 2 puts the squares at 0.05 and 0.8, off by %d", samples, sharpError ) );

	postProcessSettings_t inside = base;
	inside.localBlurredBlend = 1.0f;
	inside.localShadowContrast = 0.5f;
	const renderRect_t inner = { 16, 16, 32, 32 };
	const std::vector<uint32> confined = run( inside, inner, 4.0f, { { { { 16, 16, 48, 48 } }, 0.05f } } );
	const int insideError = worst( confined, inner, [&]( int, int ) { return encoded( contrast( 0.05, 0.5 ) ); } );
	Check( insideError <= 1, va( "%u sample(s): the scene outside the viewport stays out of its local exposure, off by %d", samples, insideError ) );

	postProcessSettings_t neutral = base;
	neutral.localBlurredBlend = 0.6f;
	postProcessSettings_t none = step;
	none.localExposure = false;
	postProcessSettings_t shown = step;
	shown.visualize = 4;
	postProcessSettings_t hidden = none;
	hidden.visualize = 4;
	const std::vector<uint32> plain = run( none, whole, 0.6f, halves );
	Check( !plain.empty() && run( neutral, whole, 0.6f, halves ) == plain && run( hidden, whole, 0.6f, halves ) == plain,
		"all 1, and visualize 4 with the local exposure off, give the frame without it" );
	const std::vector<uint32> visualized = run( shown, whole, 0.6f, halves );
	Check( !visualized.empty() && visualized != plain && visualized != stepped, "visualize 4 draws the local exposure" );
	gpu.WaitIdle();
	CheckNoLayerErrors( gpu );
}

void LocalExposureAllSamples( idTestGpu & gpu ) {
	LocalExposure( gpu, 1 );
	LocalExposure( gpu, 4 );
}

void CombineLutAllSamples( idTestGpu & gpu ) {
	CombineLut( gpu, 1 );
	CombineLut( gpu, 4 );
}

// White balance and grading are skipped at their neutral values; a warm white is made neutral by cooling.
void Test_HdrSettings() {
	const d3d12LutParms_t neutral = D3D12_LutParms( postProcessSettings_t() );
	Check( neutral.encoding[2] == 0.0f && neutral.encoding[3] == 0.0f, "the default settings skip white balance and grading" );
	Check( neutral.options[2] == static_cast<float>( hdrTonemapper_t::AGX ), "AgX by default" );
	const std::array<std::array<float, 3>, 3> same = D3D12_WhiteBalance( 6500.0f, 0.0f );
	float off = 0.0f;
	for ( int i = 0; i < 3; i++ ) {
		for ( int j = 0; j < 3; j++ ) {
			off = Max( off, idMath::Fabs( same[i][j] - ( ( i == j ) ? 1.0f : 0.0f ) ) );
		}
	}
	Check( off < 1e-5f, va( "6500 K, tint 0 is the identity (%g)", off ) );
	const auto applied = []( const std::array<std::array<float, 3>, 3> & m ) {
		return idVec3( m[0][0] + m[0][1] + m[0][2], m[1][0] + m[1][1] + m[1][2], m[2][0] + m[2][1] + m[2][2] );
	};
	const idVec3 warm = applied( D3D12_WhiteBalance( 3000.0f, 0.0f ) );
	const idVec3 cool = applied( D3D12_WhiteBalance( 10000.0f, 0.0f ) );
	Check( warm.z > warm.x && cool.x > cool.z, va( "3000 K cools white (%s), 10000 K warms it (%s)", warm.ToString( 3 ), cool.ToString( 3 ) ) );
	postProcessSettings_t graded;
	graded.global.saturation.w = 0.5f;
	graded.shadows.gain.x = 2.0f;
	const d3d12LutParms_t parms = D3D12_LutParms( graded );
	Check( parms.encoding[3] == 1.0f && parms.grade[0][3] == 0.5f && parms.grade[3][0] == 2.0f && parms.grade[8][0] == 1.0f,
		"the global saturation multiplies each range's, a range's gain stays its own" );
	Check( d3d12Lut::Value( 17.0f / 31.0f ) > 0.9999f && d3d12Lut::Value( 17.0f / 31.0f ) < 1.0001f, "1.0 lies on texel 17" );
	Check( idMath::Fabs( d3d12Lut::Coordinate( d3d12Lut::Value( 0.3f ) ) - 0.3f ) < 1e-5f, "Coordinate inverts Value" );
}

template <void ( *RULE )( idTestGpu & )>
void OnWarp() {
	if ( const std::unique_ptr<idTestGpu> gpu = CreateWarp( 16 << 20 ) ) {
		RULE( *gpu );
	}
}

template <void ( *RULE )( idTestGpu & )>
void OnGpu() {
	if ( const std::unique_ptr<idTestGpu> gpu = CreateHardware( 16 << 20 ) ) {
		RULE( *gpu );
	}
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
	{ "Warp_FrameBuffersStaged", Test_WarpFrameBuffersStaged },
	{ "Gpu_FrameBuffersGpuUpload", Test_GpuFrameBuffersGpuUpload },
	{ "Warp_LargeStage", Test_WarpLargeStage },
	{ "Warp_GravesWaitForWaited", Test_WarpGravesWaitForWaited },
	{ "Warp_RingRetiresByWaited", Test_WarpRingRetiresByWaited },
	{ "Format_Table", Test_FormatTable },
	{ "Sampler_Packing", Test_SamplerPacking },
	{ "Sampler_Map", Test_SamplerMap },
	{ "Warp_DescriptorSlots", Test_WarpDescriptorSlots },
	{ "Warp_SamplerSlots", Test_WarpSamplerSlots },
	{ "Warp_TextureFormats", Test_WarpTextureFormats },
	{ "Gpu_TextureFormats", Test_GpuTextureFormats },
	{ "Warp_Borders", Test_WarpBorders },
	{ "Gpu_Borders", Test_GpuBorders },
	{ "Warp_UnalignedBlocks", Test_WarpUnalignedBlocks },
	{ "Gpu_UnalignedBlocks", Test_GpuUnalignedBlocks },
	{ "Warp_CubeFaces", Test_WarpCubeFaces },
	{ "Warp_TextureLevels", Test_WarpTextureLevels },
	{ "Warp_TextureGraves", Test_WarpTextureGraves },
	{ "Dxc_Compile", Test_DxcCompile },
	{ "Hlsl_Bindless", Test_HlslBindless },
	{ "Hlsl_Entries", Test_HlslEntries },
	{ "Hlsl_SourcesCompile", Test_HlslSourcesCompile },
	{ "Hlsl_ProgramsMatchGl", Test_HlslProgramsMatchGl },
	{ "Hlsl_ProgramTable", Test_HlslProgramTable },
	{ "Hlsl_BuildAll", Test_HlslBuildAll },
	{ "Hlsl_TargetMacros", Test_HlslTargetMacros },
	{ "Hlsl_InternalStages", Test_HlslInternalStages },
	{ "Hlsl_Compute", Test_HlslCompute },
	{ "Hlsl_ColorSpace", OnWarp<ColorSpace> },
	{ "Warp_DxcBindless", OnWarp<DxcBindless> },
	{ "Warp_PrebuiltStages", OnWarp<PrebuiltStages> },
	{ "Gpu_DxcBindless", OnGpu<DxcBindless> },
	{ "Pipeline_Blend", Test_PipelineBlend },
	{ "Pipeline_Depth", Test_PipelineDepth },
	{ "Pipeline_Stencil", Test_PipelineStencil },
	{ "Pipeline_Raster", Test_PipelineRaster },
	{ "Pipeline_Inputs", Test_PipelineInputs },
	{ "Pipeline_Normalize", Test_PipelineNormalize },
	{ "Pipeline_DepthBiasTable", Test_DepthBiasTable },
	{ "Pipeline_Journal", Test_PipelineJournal },
	{ "Pipeline_TargetIds", Test_PipelineTargetIds },
	{ "Warp_Pipelines", Test_WarpPipelines },
	{ "Warp_PipelineWarm", OnWarp<PipelineWarm> },
	{ "Gpu_PipelineWarm", OnGpu<PipelineWarm> },
	{ "Warp_DebugGroups", Test_WarpDebugGroups },
	{ "Warp_Timestamps", OnWarp<Timestamps> },
	{ "Gpu_Timestamps", OnGpu<Timestamps> },
	{ "Warp_RasterGlRows", OnWarp<RasterGlRows> },
	{ "Gpu_RasterGlRows", OnGpu<RasterGlRows> },
	{ "Warp_RasterCull", OnWarp<RasterCull> },
	{ "Gpu_RasterCull", OnGpu<RasterCull> },
	{ "Warp_RasterDepthEqual", OnWarp<RasterDepthEqual> },
	{ "Gpu_RasterDepthEqual", OnGpu<RasterDepthEqual> },
	{ "Warp_RasterDepthOrder", OnWarp<RasterDepthOrder> },
	{ "Gpu_RasterDepthOrder", OnGpu<RasterDepthOrder> },
	{ "Warp_RasterStencilOps", OnWarp<RasterStencilOps> },
	{ "Gpu_RasterStencilOps", OnGpu<RasterStencilOps> },
	{ "Warp_RasterDepthBounds", OnWarp<RasterDepthBounds> },
	{ "Gpu_RasterDepthBounds", OnGpu<RasterDepthBounds> },
	{ "Warp_RasterDepthBias", OnWarp<RasterDepthBias> },
	{ "Gpu_RasterDepthBias", OnGpu<RasterDepthBias> },
	{ "Warp_CopyTargets", Test_WarpCopyTargets },
	{ "Warp_CopyColor", OnWarp<CopyColor1> },
	{ "Gpu_CopyColor", OnGpu<CopyColor1> },
	{ "Warp_CopyColorMsaa", OnWarp<CopyColor4> },
	{ "Gpu_CopyColorMsaa", OnGpu<CopyColor4> },
	{ "Warp_CopyDepth", OnWarp<CopyDepth1> },
	{ "Gpu_CopyDepth", OnGpu<CopyDepth1> },
	{ "Warp_CopyDepthMsaa", OnWarp<CopyDepth4> },
	{ "Gpu_CopyDepthMsaa", OnGpu<CopyDepth4> },
	{ "Warp_DeviceLoss", Test_DeviceLoss },
	{ "Output_BreadcrumbText", Test_BreadcrumbText },
	{ "Output_GammaIdentity", Test_GammaIdentity },
	{ "Output_SceneSamples", Test_SceneSamples },
	{ "Warp_PresentBlit", OnWarp<PresentBlit1> },
	{ "Gpu_PresentBlit", OnGpu<PresentBlit1> },
	{ "Warp_PresentBlitMsaa", OnWarp<PresentBlit4> },
	{ "Gpu_PresentBlitMsaa", OnGpu<PresentBlit4> },
	{ "Warp_DrawFrameBuffers", OnWarp<DrawFrameBufferPlacements> },
	{ "Warp_DrawBindless", OnWarp<DrawBindless> },
	{ "Gpu_DrawBindless", OnGpu<DrawBindless> },
	{ "Gpu_DrawFrameBuffers", OnGpu<DrawFrameBufferPlacements> },
	{ "Warp_RenderGraph", OnWarp<RenderGraph> },
	{ "Gpu_RenderGraph", OnGpu<RenderGraph> },
	{ "Hdr_Settings", Test_HdrSettings },
	{ "Warp_HdrScene", OnWarp<HdrScene> },
	{ "Gpu_HdrScene", OnGpu<HdrScene> },
	{ "Warp_HdrCopies", OnWarp<HdrCopiesAllSamples> },
	{ "Warp_CombineLut", OnWarp<CombineLutAllSamples> },
	{ "Warp_HdrEncode", OnWarp<HdrEncodeAllSamples> },
	{ "Gpu_HdrEncode", OnGpu<HdrEncodeAllSamples> },
	{ "Warp_Exposure", OnWarp<ExposureAllSamples> },
	{ "Gpu_Exposure", OnGpu<ExposureAllSamples> },
	{ "Warp_Bloom", OnWarp<BloomAllSamples> },
	{ "Gpu_Bloom", OnGpu<BloomAllSamples> },
	{ "Warp_LocalExposure", OnWarp<LocalExposureAllSamples> },
	{ "Gpu_LocalExposure", OnGpu<LocalExposureAllSamples> },
	{ "Gpu_CombineLut", OnGpu<CombineLutAllSamples> },
};

}	// namespace

int main( int argc, char ** argv ) {
	const char * filter = ( argc > 1 && idStr::Icmp( argv[1], "all" ) != 0 ) ? argv[1] : nullptr;
	if ( getenv( "D3D12_TESTS_VERBOSE" ) != nullptr ) {
		maxFailuresPerTest = 1 << 30;
	}
	idLib::common = common;	// idLib's warnings and errors go through it
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
