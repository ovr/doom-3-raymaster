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
#include "d3d12_Context.h"

idCVar r_d3d12Adapter( "r_d3d12Adapter", "", CVAR_RENDERER | CVAR_INIT,
	"Direct3D 12 adapter: empty = the high-performance GPU, warp = the software reference, N = adapter N by GPU preference" );
idCVar r_d3d12CaptureBackBuffer( "r_d3d12CaptureBackBuffer", "0", CVAR_RENDERER | CVAR_BOOL,
	"1 = ReadOutputPixels reads the back buffer after the present blit instead of the scene target (checks the blit)" );

namespace {

constexpr uint64 CONSTANTS_RING_SIZE = 16 << 20;
constexpr uint64 STAGING_RING_SIZE = 64 << 20;	// four uploader pieces: the GPU copies one while the CPU fills the next

}	// namespace

std::expected<std::unique_ptr<idD3D12Context>, idStr> idD3D12Context::Create( const renderBackendInit_t & init ) {
	auto loaded = idD3D12Library::Load();
	if ( !loaded ) {
		return std::unexpected( loaded.error() );
	}
	auto created = idD3D12Device::Create( *loaded, r_d3d12Adapter.GetString(), r_debugContext.GetInteger() );
	if ( !created ) {
		return std::unexpected( created.error() );
	}
	std::unique_ptr<idD3D12Context> context( new idD3D12Context( std::move( *loaded ), std::move( *created ) ) );
	if ( auto output = context->CreateOutput( static_cast<HWND>( init.nativeWindow ), init.width, init.height ); !output ) {
		return std::unexpected( output.error() );
	}
	context->CreateDefaults();
	context->device.DrainMessages();
	return context;
}

idD3D12Context::idD3D12Context( idD3D12Library && loadedLibrary, idD3D12Device && createdDevice ) :
	library( std::move( loadedLibrary ) ),
	device( std::move( createdDevice ) ),
	liveObjects( device ),
	queue( device ),
	graveyard( queue ),
	commands( device, queue ),
	barriers( *this ),
	srvPool( device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ),
	rtvPool( device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV ),
	dsvPool( device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV ),
	descriptors( device ),
	constants( device, CONSTANTS_RING_SIZE, L"doom3 constants ring" ),
	staging( device, STAGING_RING_SIZE, L"doom3 staging ring" ),
	uploader( device, *this, staging, barriers, graveyard ),
	buffers( device, graveyard, uploader, barriers, d3d12BufferPlacement_t::UPLOAD ),
	compiler( library ),
	rootSignature( device, library ),
	scene( device, graveyard, srvPool, rtvPool, dsvPool ),
	passes( device, rootSignature, compiler, *this, barriers, constants, descriptors, srvPool ),
	readback( device, *this, barriers, graveyard ),
	recorder( *this, barriers, scene, passes ) {
}

// Safe after a mid-frame error: the open list is closed without running. The members then die in
// reverse order, the library last.
idD3D12Context::~idD3D12Context() {
	commands.Abandon();
	barriers.Discard();
	if ( !device.IsLost() ) {
		WaitIdle();
	}
	buffers.DestroyAll();
	graveyard.CollectAll();
	device.DrainMessages();
}

// MSAA comes with the sample-count option (stage 12): one sample until then.
std::expected<void, idStr> idD3D12Context::CreateOutput( HWND window, int width, int height ) {
	auto created = idD3D12SwapChain::Create( device, queue, rtvPool, window, width, height );
	if ( !created ) {
		return std::unexpected( created.error() );
	}
	swapChain.emplace( std::move( *created ) );
	scene.Create( swapChain->Width(), swapChain->Height(), 1 );
	return {};
}

void idD3D12Context::CreateDefaults() {
	passes.CreateDefaults();
	buffers.CreateDefaults();
}

ID3D12GraphicsCommandList1 & idD3D12Context::List() {
	if ( commands.IsOpen() ) {
		return commands.List();
	}
	ID3D12GraphicsCommandList1 & list = commands.Open();
	ID3D12DescriptorHeap * heaps[] = { descriptors.Heap() };
	list.SetDescriptorHeaps( 1, heaps );
	list.SetGraphicsRootSignature( rootSignature.Get() );
	recorder.Invalidate();
	return list;
}

uint64 idD3D12Context::SubmitChunk() {
	if ( !commands.IsOpen() ) {
		return queue.LastSignaled();
	}
	barriers.Flush();
	const uint64 fence = commands.CloseAndExecute();
	constants.CloseChunk( fence );
	staging.CloseChunk( fence );
	descriptors.CloseChunk( fence );
	frame.submits++;
	return fence;
}

void idD3D12Context::WaitFor( uint64 fence ) {
	if ( fence > queue.Waited() ) {
		frame.waits++;
	}
	queue.Wait( fence );
	Retire( queue.Waited() );
}

// The fence of the last list comes before the Present that still reads the back buffer.
void idD3D12Context::WaitIdle() {
	SubmitChunk();
	WaitFor( queue.Signal() );
}

void idD3D12Context::Retire( uint64 waitedFence ) {
	constants.Retire( waitedFence );
	staging.Retire( waitedFence );
	descriptors.Retire( waitedFence );
	graveyard.Collect();
}

// Dragging the border resizes the window without ResizeOutput; nothing is recorded yet, so the
// output can follow here.
void idD3D12Context::BeginFrame( int width, int height ) {
	if ( swapChain && ( width != swapChain->Width() || height != swapChain->Height() ) ) {
		Resize( width, height );
	}
}

void idD3D12Context::EndFrame() {
	recorder.EndPass();
	SubmitChunk();
}

void idD3D12Context::BlitToBackBuffer( idD3D12SwapChain::backBuffer_t & back ) {
	recorder.EndPass();
	idD3D12SceneTarget::surface_t & source = scene.Resolved( barriers, *this );
	barriers.Transition( *source.resource.Get(), source.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	barriers.Transition( *back.resource.Get(), back.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	passes.PresentBlit( source.srv, rtvPool.Cpu( back.rtv ), scene.Width(), scene.Height() );
	recorder.Invalidate();
}

// Blocks until the GPU has finished the frame: the next frame maps the vertex cache set this one read.
presentResult_t idD3D12Context::Present( presentMode_t mode ) {
	if ( device.IsLost() || !swapChain ) {
		return presentResult_t::DEVICE_LOST;
	}
	idD3D12SwapChain::backBuffer_t & back = swapChain->Current();
	BlitToBackBuffer( back );
	barriers.Transition( *back.resource.Get(), back.state, D3D12_RESOURCE_STATE_PRESENT );
	const uint64 fence = SubmitChunk();

	const presentResult_t result = swapChain->Present( mode );
	WaitFor( fence );
	swapChain->WaitForLatency();
	EndFrameStats();
	return device.IsLost() ? presentResult_t::DEVICE_LOST : result;
}

void idD3D12Context::EndFrameStats() {
	frame.barriers = barriers.IssuedThisFrame();
	frame.uploadBytes = uploader.BytesThisFrame();
	frame.inPlaceBytes = buffers.InPlaceBytesThisFrame();
	lastFrame = frame;
	frame = {};
	barriers.ResetFrameCounters();
	uploader.ResetFrameCounters();
	buffers.ResetFrameCounters();
}

// r_d3d12CaptureBackBuffer reads what Present would show, so a wrong row flip in the blit makes the
// capture differ from the scene's; Present then blits into the same back buffer again.
void idD3D12Context::ReadOutputPixels( int x, int y, int width, int height, byte * rgb ) {
	recorder.EndPass();
	if ( r_d3d12CaptureBackBuffer.GetBool() && swapChain ) {
		idD3D12SwapChain::backBuffer_t & back = swapChain->Current();
		BlitToBackBuffer( back );
		readback.Read( *back.resource.Get(), back.state, x, y, width, height, rgb, true );
		return;
	}
	idD3D12SceneTarget::surface_t & source = scene.Resolved( barriers, *this );
	readback.Read( *source.resource.Get(), source.state, x, y, width, height, rgb, false );
}

void idD3D12Context::Resize( int width, int height ) {
	if ( width <= 0 || height <= 0 || !swapChain ) {
		return;
	}
	if ( width == swapChain->Width() && height == swapChain->Height() ) {
		return;
	}
	WaitIdle();
	swapChain->Resize( width, height );
	scene.Create( swapChain->Width(), swapChain->Height(), scene.Samples() );
	recorder.Invalidate();
	common->DPrintf( "d3d12: output resized to %d x %d\n", width, height );
}

void idD3D12Context::PrintInfo() const {
	device.PrintInfo();
	const uint64 version = library.compilerVersion;
	common->Printf( "D3D12 shader compiler: d3dcompiler_47.dll %u.%u.%u.%u, %d shaders compiled\n", static_cast<unsigned>( version >> 48 ),
		static_cast<unsigned>( ( version >> 32 ) & 0xFFFF ), static_cast<unsigned>( ( version >> 16 ) & 0xFFFF ), static_cast<unsigned>( version & 0xFFFF ),
		compiler.Compiled() );
	if ( swapChain ) {
		common->Printf( "D3D12 swap chain: %d x %d, %u flip-discard buffers\n", swapChain->Width(), swapChain->Height(), idD3D12SwapChain::BUFFERS );
	}
	common->Printf( "D3D12 scene target: %d x %d, RGBA8 + D24S8, %u sample(s); constants ring %llu MB, staging ring %llu MB\n", scene.Width(),
		scene.Height(), scene.Samples(), static_cast<unsigned long long>( constants.Size() >> 20 ), static_cast<unsigned long long>( staging.Size() >> 20 ) );
	common->Printf( "D3D12 buffers: %d, FRAME %.1f MB in UPLOAD heaps, STATIC %.1f MB in DEFAULT heaps + %.1f MB CPU mirrors\n", buffers.Num(),
		buffers.Bytes( bufferUsage_t::FRAME ) / ( 1024.0 * 1024.0 ), buffers.Bytes( bufferUsage_t::STATIC ) / ( 1024.0 * 1024.0 ),
		buffers.MirrorBytes() / ( 1024.0 * 1024.0 ) );
}
