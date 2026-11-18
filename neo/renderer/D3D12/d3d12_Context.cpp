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
idCVar r_d3d12ShaderCache( "r_d3d12ShaderCache", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_INIT,
	"1 = Direct3D 12 keeps the DXBC of every shader in fs_savepath/renderprogs/dxbc and compiles only a text it has not seen" );

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
	timers( device, queue ),
	barriers( *this ),
	srvPool( device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ),
	rtvPool( device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV ),
	dsvPool( device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV ),
	descriptors( device ),
	samplers( device ),
	constants( device, CONSTANTS_RING_SIZE, L"doom3 constants ring" ),
	staging( device, STAGING_RING_SIZE, L"doom3 staging ring" ),
	uploader( device, *this, staging, barriers, graveyard ),
	buffers( device, graveyard, uploader, barriers, d3d12BufferPlacement_t::UPLOAD ),
	textures( device, graveyard, srvPool, rtvPool, uploader, barriers, device.Features().unalignedBlocks ),
	compiler( library, r_d3d12ShaderCache.GetBool() ),
	shaders( compiler, graveyard ),
	rootSignature( device, library ),
	pipelines( device, rootSignature ),
	scene( device, graveyard, srvPool, rtvPool, dsvPool ),
	passes( device, rootSignature, compiler, *this, barriers, constants, descriptors, samplers, srvPool ),
	readback( device, *this, barriers, graveyard ),
	copies( *this, barriers, scene, textures, passes, rtvPool ),
	recorder( device, *this, barriers, scene, passes, pipelines, shaders, buffers, textures, srvPool, constants, descriptors, samplers ) {
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
	textures.DestroyAll();
	shaders.DestroyAll();
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
	textures.CreateDefaults();
	passes.CreateDefaults( textures.Fallback( textureKind_t::TEX_2D ), textures.Fallback( textureKind_t::TEX_CUBE ) );
	buffers.CreateDefaults();
}

ID3D12GraphicsCommandList1 & idD3D12Context::List() {
	if ( commands.IsOpen() ) {
		return commands.List();
	}
	ID3D12GraphicsCommandList1 & list = commands.Open();
	ID3D12DescriptorHeap * heaps[] = { descriptors.Heap(), samplers.Heap() };
	list.SetDescriptorHeaps( 2, heaps );
	list.SetGraphicsRootSignature( rootSignature.Get() );
	recorder.NewList();
	groups.Reopen( list );
	return list;
}

uint64 idD3D12Context::SubmitChunk() {
	if ( !commands.IsOpen() ) {
		return queue.LastSignaled();
	}
	barriers.Flush();
	groups.CloseAll( commands.List() );
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

// Dragging the border resizes the window without ResizeOutput; nothing of the frame is recorded yet,
// so the output can follow here. The frame timer's first stamp then opens the frame's list.
void idD3D12Context::BeginFrame( int width, int height ) {
	if ( swapChain && ( width != swapChain->Width() || height != swapChain->Height() ) ) {
		Resize( width, height );
	}
	recorder.BeginFrame();
	if ( !device.IsLost() ) {
		timers.BeginFrame( List() );
	}
}

// The resolves go into the list submitted here, so the Present that follows waits for them too.
void idD3D12Context::EndFrame() {
	recorder.EndPass();
	if ( !device.IsLost() ) {
		timers.EndFrame( List() );
	}
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
	frame.draw = recorder.TakeStats();
	frame.psoCreated = pipelines.CreatedThisFrame();
	frame.nearestBiases = pipelines.Bias().NearestThisFrame();
	frame.barriers = barriers.IssuedThisFrame();
	frame.uploadBytes = uploader.BytesThisFrame();
	frame.inPlaceBytes = buffers.InPlaceBytesThisFrame();
	frame.samplerTables = samplers.BuiltThisFrame();
	frame.debugGroups = groups.PushedThisFrame();
	frame.timestamps = timers.WrittenThisFrame();
	frame.gpuMicroseconds = timers.FrameMicroseconds();
	const idD3D12ScreenCopies::stats_t copied = copies.TakeStats();
	frame.colorCopies = copied.color;
	frame.depthCopies = copied.depth;
	frame.copySurfaces = textures.CopySurfacesThisFrame();
	lastFrame = frame;
	frame = {};
	textures.ResetFrameCounters();
	pipelines.ResetFrameCounters();
	barriers.ResetFrameCounters();
	uploader.ResetFrameCounters();
	buffers.ResetFrameCounters();
	samplers.ResetFrameCounters();
	groups.ResetFrameCounters();
	timers.ResetFrameCounters();
}

void idD3D12Context::PushGroup( const char * name ) {
	groups.Push( List(), name );
}

void idD3D12Context::PopGroup() {
	groups.Pop( commands.IsOpen() ? &commands.List() : nullptr );
}

// Over everything the frame drew, in the scene target, so ReadOutputPixels captures it.
void idD3D12Context::ShowImage( textureHandle_t handle ) {
	const d3d12Texture_t * texture = textures.Find( handle );
	if ( texture == nullptr || !swapChain ) {
		return;
	}
	recorder.EndPass();
	textures.MakeReadable( handle );
	idD3D12SceneTarget::surface_t & color = scene.Color();
	barriers.Transition( *color.resource.Get(), color.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	passes.ShowImage( textures.SampledView( handle, texture->kind ), texture->kind, texture->surface.width, texture->surface.height,
		scene.Rtv( false ), scene.Target( false ), scene.Width(), scene.Height() );
	recorder.Invalidate();
}

void idD3D12Context::CopyOutputToTexture( textureHandle_t texture, int x, int y, int width, int height ) {
	if ( device.IsLost() ) {
		return;
	}
	recorder.EndPass();
	copies.Color( texture, x, y, width, height );
}

void idD3D12Context::CopyDepthToTexture( textureHandle_t texture, int x, int y, int width, int height ) {
	if ( device.IsLost() ) {
		return;
	}
	recorder.EndPass();
	copies.Depth( texture, x, y, width, height );
	recorder.Invalidate();
}

d3d12TextureCheckResult_t idD3D12Context::CheckTextures() {
	recorder.EndPass();
	idD3D12TextureCheck check( device, *this, barriers, graveyard );
	return check.Run( textures );
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
	common->Printf( "D3D12 shader compiler: d3dcompiler_47.dll %u.%u.%u.%u, compiled %d, %d from the cache; %d stages, %d programs\n",
		static_cast<unsigned>( version >> 48 ), static_cast<unsigned>( ( version >> 32 ) & 0xFFFF ), static_cast<unsigned>( ( version >> 16 ) & 0xFFFF ),
		static_cast<unsigned>( version & 0xFFFF ), compiler.Compiled(), compiler.CacheHits(), shaders.NumStages(), shaders.NumPrograms() );
	common->Printf( "D3D12 pipelines: %d PSOs created, %d depth biases; depth bounds %s\n", pipelines.Num(), pipelines.Bias().Num(),
		device.Features().depthBounds ? "in every PSO" : "unsupported" );
	common->Printf( "D3D12 GPU timers: %s, %.3f MHz; the frame timer and %d x %d r_pix slots\n", timers.Available() ? "on" : "off",
		static_cast<double>( timers.Frequency() ) / 1.0e6, TIMESTAMP_SETS, idD3D12GpuTimers::SLOTS_PER_SET );
	if ( swapChain ) {
		common->Printf( "D3D12 swap chain: %d x %d, %u flip-discard buffers\n", swapChain->Width(), swapChain->Height(), idD3D12SwapChain::BUFFERS );
	}
	common->Printf( "D3D12 scene target: %d x %d, RGBA8 + D24S8, %u sample(s); constants ring %llu MB, staging ring %llu MB\n", scene.Width(),
		scene.Height(), scene.Samples(), static_cast<unsigned long long>( constants.Size() >> 20 ), static_cast<unsigned long long>( staging.Size() >> 20 ) );
	common->Printf( "D3D12 buffers: %d, FRAME %.1f MB in UPLOAD heaps, STATIC %.1f MB in DEFAULT heaps + %.1f MB CPU mirrors\n", buffers.Num(),
		buffers.Bytes( bufferUsage_t::FRAME ) / ( 1024.0 * 1024.0 ), buffers.Bytes( bufferUsage_t::STATIC ) / ( 1024.0 * 1024.0 ),
		buffers.MirrorBytes() / ( 1024.0 * 1024.0 ) );
	common->Printf( "D3D12 textures: %d, %.1f MB; sampler tables %d of %u; unaligned BC %s, anisotropic point mips %s\n", textures.Num(),
		textures.Bytes() / ( 1024.0 * 1024.0 ), samplers.Used(), idD3D12SamplerHeap::TABLES, device.Features().unalignedBlocks ? "yes" : "decoded",
		device.Features().anisoPointMip ? "yes" : "no" );
}
